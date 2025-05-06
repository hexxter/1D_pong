#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rmt.h" // Kept as per your request, though led_strip.h abstracts its use
#include "led_strip.h"
#include <stdio.h>
#include "esp_log.h"
#include <stdbool.h> // For bool type
#include <inttypes.h> // For PRIu32 in ESP_LOG

static const char *TAG = "PongGame";

#define NUM_LEDS 54
#define LED_PIN GPIO_NUM_16
#define BUTTON1_PIN GPIO_NUM_25 // Player Left
#define BUTTON2_PIN GPIO_NUM_27 // Player Right

#define PADDLE_SIZE 6     // Number of LEDs for the paddle (as seen in video)
#define INITIAL_LIVES 5
#define INITIAL_BALL_SPEED 0.4f   // Start speed (LEDs per update-cycle) - adjusted for feel
#define BALL_SPEED_INCREMENT 0.05f // Speed increase per hit - adjusted
#define BALL_UPDATE_INTERVAL_MS 40 // How often the ball logically moves (ms)
#define GAME_LOOP_DELAY_MS 10      // Main loop delay (ms)

typedef enum {
    LEFT,
    RIGHT,
    STOP
} direction_type;

typedef enum {
    GAME_STATE_INIT,          // Game initializing / Start screen
    GAME_STATE_WAIT_SERVE,    // Waiting for serve
    GAME_STATE_PLAYING,       // Ball is in play
    GAME_STATE_POINT_SCORED,  // Point scored, brief pause
    GAME_STATE_GAME_OVER      // Game over animation
} GameState;

typedef struct {
    gpio_num_t pin;
    bool currentState;
    bool lastState;
    bool justPressed;
} Button;

typedef struct {
    uint8_t lives;
    direction_type side; // LEFT or RIGHT
    uint32_t color;
    int paddle_pos_start; // For rendering
    int paddle_pos_end;   // For rendering
} Player;

typedef struct {
    float position;
    direction_type direction;
    float speed;
    uint32_t color;
} Ball;

Button button_p1, button_p2;
Player player1, player2;
Ball ball;
GameState currentGameState = GAME_STATE_INIT;
Player *servingPlayer; // Pointer to the player who serves

// --- Color Definitions (RGB) ---
uint32_t colorToUint32(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

rgb_t uint32ToRgb(uint32_t color) {
    return (rgb_t){
        .r = (uint8_t)((color >> 16) & 0xFF),
        .g = (uint8_t)((color >> 8) & 0xFF),
        .b = (uint8_t)(color & 0xFF)
    };
}

const uint32_t COLOR_BLACK = 0x000000;
const uint32_t COLOR_WHITE = 0xFFFFFF;
const uint32_t COLOR_RED = 0xFF0000;
const uint32_t COLOR_GREEN = 0x00FF00;
const uint32_t COLOR_BLUE = 0x0000FF;
const uint32_t COLOR_P1_PADDLE = 0xFF0000; // Red for P1 (left button in video)
const uint32_t COLOR_P2_PADDLE = 0x00FF00; // Green for P2 (right button in video)
const uint32_t COLOR_BALL = 0x00FFFF;   // Cyan ball (was white, changed for visibility)
const uint32_t COLOR_LIFE_ACTIVE = 0xFFFF00; // Yellow for active lives
const uint32_t COLOR_LIFE_LOST = 0x400000; // Dim Red for lost lives

led_strip_t strip;

// --- LED Strip Functions ---
void init_led_strip() {
    strip.type = LED_STRIP_WS2812;
    strip.length = NUM_LEDS;
    strip.gpio = LED_PIN;
    strip.buf = NULL; // Buffer will be allocated by the library
    strip.brightness = 60; // Reduce brightness (0-255)

    led_strip_install(); // Call this first!
    ESP_ERROR_CHECK(led_strip_init(&strip));
    ESP_ERROR_CHECK(led_strip_flush(&strip)); // Turn all LEDs off
    ESP_LOGI(TAG, "LED strip initialized.");
}

void set_pixel_color(int index, uint32_t color_val) {
    if (index >= 0 && index < NUM_LEDS) {
        ESP_ERROR_CHECK(led_strip_set_pixel(&strip, index, uint32ToRgb(color_val)));
    }
}

void fill_color(uint32_t color_val) {
    for (int i = 0; i < NUM_LEDS; i++) {
        set_pixel_color(i, color_val);
    }
}

// --- Button Functions ---
void init_buttons() {
    button_p1.pin = BUTTON1_PIN;
    button_p2.pin = BUTTON2_PIN;
    Button *buttons[] = {&button_p1, &button_p2};

    for (int i = 0; i < 2; i++) {
        gpio_reset_pin(buttons[i]->pin);
        gpio_set_direction(buttons[i]->pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(buttons[i]->pin, GPIO_PULLUP_ONLY); // Assuming buttons pull to GND when pressed
        buttons[i]->currentState = gpio_get_level(buttons[i]->pin); // Initialize with current level
        buttons[i]->lastState = buttons[i]->currentState;
        buttons[i]->justPressed = false;
    }
    ESP_LOGI(TAG, "Buttons initialized.");
}

void update_button(Button *button) {
    button->lastState = button->currentState;
    button->currentState = !gpio_get_level(button->pin); // Active low due to PULLUP_ONLY
    button->justPressed = button->currentState && !button->lastState;
}

void process_input() {
    update_button(&button_p1);
    update_button(&button_p2);
}

// --- Game Initialization ---
void init_game_elements() {
    player1.side = LEFT;
    player1.lives = INITIAL_LIVES;
    player1.color = COLOR_P1_PADDLE;
    player1.paddle_pos_start = 0;
    player1.paddle_pos_end = PADDLE_SIZE - 1;

    player2.side = RIGHT;
    player2.lives = INITIAL_LIVES;
    player2.color = COLOR_P2_PADDLE;
    player2.paddle_pos_start = NUM_LEDS - PADDLE_SIZE;
    player2.paddle_pos_end = NUM_LEDS - 1;

    ball.color = COLOR_BALL;
    ball.speed = INITIAL_BALL_SPEED;

    // Alternate starting player or P1 starts
    if (servingPlayer == &player1) {
        servingPlayer = &player2;
    } else {
        servingPlayer = &player1;
    }
    // servingPlayer = &player1; // Player 1 always starts first game
    
    ESP_LOGI(TAG, "Game elements initialized. Player %s serves.", (servingPlayer == &player1) ? "1 (Left)" : "2 (Right)");
}

void prepare_serve() {
    ball.speed = INITIAL_BALL_SPEED; // Reset speed for new serve
    if (servingPlayer->side == LEFT) {
        ball.position = player1.paddle_pos_end + 1.0f; // Just in front of the paddle
        ball.direction = STOP; // Waits for action
    } else {
        ball.position = player2.paddle_pos_start - 1.0f;
        ball.direction = STOP;
    }
    currentGameState = GAME_STATE_WAIT_SERVE;
    ESP_LOGI(TAG, "Prepare serve. Ball at %.1f, Player %s to serve.", ball.position, (servingPlayer == &player1) ? "1" : "2");
}

// --- Game Logic ---
void update_ball_position() {
    if (ball.direction == LEFT) {
        ball.position -= ball.speed;
    } else if (ball.direction == RIGHT) {
        ball.position += ball.speed;
    }

    int ball_led_idx = (int)(ball.position + 0.5f); // Round to nearest LED

    // Collision with Walls (Points)
    if (ball.position < 0) { // Ball went past player 1
        ESP_LOGI(TAG, "Ball out on left. Player 2 scores.");
        player1.lives--;
        servingPlayer = &player1; // Loser serves
        currentGameState = GAME_STATE_POINT_SCORED;
        return; // Exit early as point is scored
    } else if (ball.position >= NUM_LEDS -1) { // Ball went past player 2
        ESP_LOGI(TAG, "Ball out on right. Player 1 scores.");
        player2.lives--;
        servingPlayer = &player2; // Loser serves
        currentGameState = GAME_STATE_POINT_SCORED;
        return; // Exit early
    }

    // Collision with Paddles
    // Player 1 (left)
    if (ball.direction == LEFT && ball_led_idx <= player1.paddle_pos_end && ball_led_idx >= player1.paddle_pos_start) {
        if (button_p1.currentState) { // Paddle active (button held)
            ESP_LOGI(TAG, "Player 1 hit! Ball at %d, Paddle [%d-%d]", ball_led_idx, player1.paddle_pos_start, player1.paddle_pos_end);
            ball.direction = RIGHT;
            ball.position = player1.paddle_pos_end + 0.1f; // Move slightly out of paddle
            ball.speed += BALL_SPEED_INCREMENT;
            if (ball.speed > 1.5f) ball.speed = 1.5f; // Max speed
            ESP_LOGI(TAG, "New ball speed: %.2f", ball.speed);
        }
    }
    // Player 2 (right)
    else if (ball.direction == RIGHT && ball_led_idx >= player2.paddle_pos_start && ball_led_idx <= player2.paddle_pos_end) {
         if (button_p2.currentState) { // Paddle active (button held)
            ESP_LOGI(TAG, "Player 2 hit! Ball at %d, Paddle [%d-%d]", ball_led_idx, player2.paddle_pos_start, player2.paddle_pos_end);
            ball.direction = LEFT;
            ball.position = player2.paddle_pos_start - 0.1f; // Move slightly out of paddle
            ball.speed += BALL_SPEED_INCREMENT;
            if (ball.speed > 1.5f) ball.speed = 1.5f; // Max speed
            ESP_LOGI(TAG, "New ball speed: %.2f", ball.speed);
        }
    }
}

void rainbowCycle(int wait_ms, int cycles) {
    uint16_t i, j;
    for (j = 0; j < 256 * cycles; j++) { // 5 cycles of all colors on wheel
        if (currentGameState != GAME_STATE_INIT && currentGameState != GAME_STATE_GAME_OVER) return;
        for (i = 0; i < NUM_LEDS; i++) {
            uint8_t r, g, b;
            uint8_t wheelpos = ((i * 256 / NUM_LEDS) + j) & 255;
            if (wheelpos < 85) { // R to G
                r = 255 - wheelpos * 3;
                g = wheelpos * 3;
                b = 0;
            } else if (wheelpos < 170) { // G to B
                wheelpos -= 85;
                r = 0;
                g = 255 - wheelpos * 3;
                b = wheelpos * 3;
            } else { // B to R
                wheelpos -= 170;
                r = wheelpos * 3;
                g = 0;
                b = 255 - wheelpos * 3;
            }
            set_pixel_color(i, colorToUint32(r,g,b));
        }
        led_strip_flush(&strip);
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}


void knightRiderAnimation(uint32_t color, int width, int repeats, int anim_speed_ms) {
    ESP_LOGI(TAG, "Knight Rider Animation Start");
    for (int r = 0; r < repeats; r++) {
        // Forward
        for (int i = 0; i <= NUM_LEDS - width; i++) {
            if (currentGameState != GAME_STATE_INIT && currentGameState != GAME_STATE_GAME_OVER) {
                ESP_LOGI(TAG, "Knight Rider interrupted by state change.");
                return;
            }
            fill_color(COLOR_BLACK);
            for (int k = 0; k < width; k++) {
                set_pixel_color(i + k, color);
            }
            led_strip_flush(&strip);
            vTaskDelay(pdMS_TO_TICKS(anim_speed_ms));
        }
        // Backward
        for (int i = NUM_LEDS - width -1; i >= 0; i--) {
             if (currentGameState != GAME_STATE_INIT && currentGameState != GAME_STATE_GAME_OVER) {
                ESP_LOGI(TAG, "Knight Rider interrupted by state change.");
                return;
            }
            fill_color(COLOR_BLACK);
            for (int k = 0; k < width; k++) {
                set_pixel_color(i + k, color);
            }
            led_strip_flush(&strip);
            vTaskDelay(pdMS_TO_TICKS(anim_speed_ms));
        }
    }
    ESP_LOGI(TAG, "Knight Rider Animation End");
}

void game_update_logic() {
    static uint32_t last_ball_update_time = 0;
    uint32_t current_time_ms = esp_log_timestamp();

    switch (currentGameState) {
        case GAME_STATE_INIT:
            ESP_LOGI(TAG, "State: GAME_STATE_INIT");
            // knightRiderAnimation(COLOR_RED, 5, 1, 30); // Start animation
            rainbowCycle(10, 2);
            init_game_elements(); // Sets lives, player data
            prepare_serve();      // Sets ball position, direction=STOP, and state to WAIT_SERVE
            break;

        case GAME_STATE_WAIT_SERVE:
            // Ball is positioned by prepare_serve(), waiting for button
            if (servingPlayer == &player1 && button_p1.justPressed) {
                ball.direction = RIGHT;
                currentGameState = GAME_STATE_PLAYING;
                ESP_LOGI(TAG, "Player 1 serves right.");
            } else if (servingPlayer == &player2 && button_p2.justPressed) {
                ball.direction = LEFT;
                currentGameState = GAME_STATE_PLAYING;
                ESP_LOGI(TAG, "Player 2 serves left.");
            }
            // Show serving player's paddle blinking
            if ((current_time_ms / 250) % 2 == 0) { // Blink every 250ms
                if (servingPlayer == &player1) set_pixel_color(player1.paddle_pos_start + PADDLE_SIZE/2, player1.color);
                else set_pixel_color(player2.paddle_pos_start + PADDLE_SIZE/2, player2.color);
            } else {
                 if (servingPlayer == &player1) set_pixel_color(player1.paddle_pos_start + PADDLE_SIZE/2, COLOR_BLACK);
                 else set_pixel_color(player2.paddle_pos_start + PADDLE_SIZE/2, COLOR_BLACK);
            }
            break;

        case GAME_STATE_PLAYING:
            if ((current_time_ms - last_ball_update_time) >= BALL_UPDATE_INTERVAL_MS) {
                update_ball_position();
                last_ball_update_time = current_time_ms;
            }
            // Check for game over directly if a point was scored by update_ball_position
            if (currentGameState == GAME_STATE_POINT_SCORED) { // update_ball_position might change state
                 // Fall through to POINT_SCORED logic below
            } else if (player1.lives == 0 || player2.lives == 0) {
                currentGameState = GAME_STATE_GAME_OVER;
            }
            break;

        case GAME_STATE_POINT_SCORED:
            ESP_LOGI(TAG, "State: GAME_STATE_POINT_SCORED. P1 Lives: %d, P2 Lives: %d", player1.lives, player2.lives);
            fill_color(COLOR_BLACK); // All off

            Player *scorer = (servingPlayer == &player1) ? &player2 : &player1; // Scorer is the one NOT serving next
            int start_led = (scorer == &player1) ? 0 : NUM_LEDS / 2;
            int end_led = (scorer == &player1) ? NUM_LEDS / 2 : NUM_LEDS;

            for(int i=0; i<3; i++){ // Blink scorer's side
                for(int j=start_led; j < end_led; j++) set_pixel_color(j, scorer->color);
                led_strip_flush(&strip);
                vTaskDelay(pdMS_TO_TICKS(200));
                for(int j=start_led; j < end_led; j++) set_pixel_color(j, COLOR_BLACK);
                led_strip_flush(&strip);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            vTaskDelay(pdMS_TO_TICKS(600)); // Longer pause after animation

            if (player1.lives == 0 || player2.lives == 0) {
                currentGameState = GAME_STATE_GAME_OVER;
            } else {
                prepare_serve(); // Prepares next serve and sets state to WAIT_SERVE
            }
            break;

        case GAME_STATE_GAME_OVER:
            ESP_LOGI(TAG, "State: GAME_STATE_GAME_OVER!");
            uint32_t winner_color = (player1.lives > 0) ? player1.color : player2.color;
            const char* winner_text = (player1.lives > 0) ? "Player 1" : "Player 2";
            ESP_LOGI(TAG, "%s WINS!", winner_text);

            // Flash winner color
            for(int i=0; i<5; i++){
                fill_color(winner_color);
                led_strip_flush(&strip);
                vTaskDelay(pdMS_TO_TICKS(250));
                fill_color(COLOR_BLACK);
                led_strip_flush(&strip);
                vTaskDelay(pdMS_TO_TICKS(250));
                 if (button_p1.justPressed || button_p2.justPressed) break; // Allow early exit
            }
            rainbowCycle(15, 3); // Victory lap!

            // Wait for a button press to restart
            ESP_LOGI(TAG, "Press any button to restart.");
            bool button_pressed_for_restart = false;
            while(!button_pressed_for_restart) {
                process_input(); // Keep updating button states
                if (button_p1.justPressed || button_p2.justPressed) {
                    button_pressed_for_restart = true;
                }
                vTaskDelay(pdMS_TO_TICKS(50)); // Check for button press periodically
            }
            currentGameState = GAME_STATE_INIT; // Back to start
            break;
    }
}

// --- Rendering ---
void render_paddles_and_lives() {
    // Player 1 Paddle
    for (int i = 0; i < PADDLE_SIZE; i++) {
        set_pixel_color(player1.paddle_pos_start + i, player1.color);
    }
    // Player 1 Lives (display next to paddle)
    // Display active lives first, then lost lives
    int life_led_idx_p1 = player1.paddle_pos_end + 2; // Start lives display 1 LED away from paddle
    for (int i = 0; i < INITIAL_LIVES; i++) {
        if (life_led_idx_p1 + i < NUM_LEDS / 2 - PADDLE_SIZE) { // Ensure lives don't overlap P2 area
            if (i < player1.lives) {
                set_pixel_color(life_led_idx_p1 + i, COLOR_LIFE_ACTIVE);
            } else {
                set_pixel_color(life_led_idx_p1 + i, COLOR_LIFE_LOST);
            }
        }
    }

    // Player 2 Paddle
    for (int i = 0; i < PADDLE_SIZE; i++) {
        set_pixel_color(player2.paddle_pos_start + i, player2.color);
    }
    // Player 2 Lives
    int life_led_idx_p2 = player2.paddle_pos_start - 2; // Start lives display 1 LED away from paddle
    for (int i = 0; i < INITIAL_LIVES; i++) {
         if (life_led_idx_p2 - i > NUM_LEDS / 2 + PADDLE_SIZE) { // Ensure lives don't overlap P1 area
            if (i < player2.lives) {
                set_pixel_color(life_led_idx_p2 - i, COLOR_LIFE_ACTIVE);
            } else {
                set_pixel_color(life_led_idx_p2 - i, COLOR_LIFE_LOST);
            }
        }
    }
}

void render_ball() {
    // Render ball only if it's in play or waiting for serve
    if (currentGameState == GAME_STATE_PLAYING || currentGameState == GAME_STATE_WAIT_SERVE) {
        int ball_led_idx = (int)(ball.position + 0.5f); // Round to nearest LED
        if (ball_led_idx >=0 && ball_led_idx < NUM_LEDS) {
             set_pixel_color(ball_led_idx, ball.color);
        }
    }
}

void draw_game() {
    // These states handle their own full-strip animations/displays
    if (currentGameState == GAME_STATE_INIT || 
        currentGameState == GAME_STATE_GAME_OVER || 
        currentGameState == GAME_STATE_POINT_SCORED) {
        return;
    }

    fill_color(COLOR_BLACK); // Clear background for dynamic elements
    render_paddles_and_lives();
    render_ball();
    
    // Blinking effect for serving player in WAIT_SERVE is handled in game_update_logic
    // but we need to ensure the paddle is drawn consistently otherwise.
    if (currentGameState == GAME_STATE_WAIT_SERVE) {
        // Re-draw the serving player's paddle center to ensure it's visible
        // (as the blinking logic in update might turn it off just before flush)
        uint32_t current_time_ms = esp_log_timestamp();
        bool show_blink = (current_time_ms / 250) % 2 == 0;
        if (servingPlayer == &player1) {
            set_pixel_color(player1.paddle_pos_start + PADDLE_SIZE/2, show_blink ? player1.color : COLOR_BLACK);
        } else {
            set_pixel_color(player2.paddle_pos_start + PADDLE_SIZE/2, show_blink ? player2.color : COLOR_BLACK);
        }
    }


    led_strip_flush(&strip);
}

// --- Main Task ---
void game_task(void *pvParameters) {
    ESP_LOGI(TAG, "Game task started.");
    init_led_strip();
    init_buttons();
    
    currentGameState = GAME_STATE_INIT; // Initial state

    while (true) {
        process_input();        // Read button states
        game_update_logic();    // Update game state machine and entity logic
        draw_game();            // Render current game state to LEDs
        vTaskDelay(pdMS_TO_TICKS(GAME_LOOP_DELAY_MS));
    }
}

void app_main(void) {
    esp_log_level_set(TAG, ESP_LOG_INFO); // Set log level for this tag
    // esp_log_level_set("*", ESP_LOG_ERROR); // Optionally, reduce general ESP-IDF logging
    
    xTaskCreate(game_task, "game_task", 4096 * 2, NULL, 5, NULL); // Increased stack for safety
}