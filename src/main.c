#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rmt.h" // Behalte ich drin, falls du es für RMT direkt brauchst
#include "led_strip.h"
#include <stdio.h>
#include "esp_log.h"
#include <stdbool.h> // Für bool Typ

static const char *TAG = "PongGame";

#define NUM_LEDS 54
#define LED_PIN GPIO_NUM_16
#define BUTTON1_PIN GPIO_NUM_25 // Spieler Links
#define BUTTON2_PIN GPIO_NUM_27 // Spieler Rechts

#define PADDLE_SIZE 3 // Anzahl LEDs für den Schläger
#define INITIAL_LIVES 5
#define INITIAL_BALL_SPEED 0.5f // Startgeschwindigkeit (LEDs pro Update-Zyklus)
#define BALL_SPEED_INCREMENT 0.1f // Geschwindigkeitserhöhung pro Treffer
#define BALL_UPDATE_INTERVAL_MS 50 // Wie oft der Ball logisch bewegt wird
#define GAME_LOOP_DELAY_MS 10      // Haupt-Schleifen-Verzögerung

typedef enum {
    LEFT,
    RIGHT,
    STOP
} direction_type;

typedef enum {
    GAME_STATE_INIT,          // Spiel wird initialisiert / Startbildschirm
    GAME_STATE_WAIT_SERVE,    // Wartet auf Aufschlag
    GAME_STATE_PLAYING,       // Ball ist im Spiel
    GAME_STATE_POINT_SCORED,  // Punkt wurde erzielt, kurze Pause
    GAME_STATE_GAME_OVER      // Spielende-Animation
} GameState;

typedef struct {
    gpio_num_t pin;
    bool currentState;
    bool lastState;
    bool justPressed;
} Button;

typedef struct {
    uint8_t lives;
    direction_type side; // LEFT oder RIGHT
    uint32_t color;
    int paddle_pos_start; // Für Rendering
    int paddle_pos_end;   // Für Rendering
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
Player *servingPlayer; // Zeiger auf den Spieler, der aufschlägt

// Farben (RGB)
uint32_t colorToUint32(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

rgb_t uint32ToRgb(uint32_t color) {
    return (rgb_t){
        .r = (color >> 16) & 0xFF,
        .g = (color >> 8) & 0xFF,
        .b = color & 0xFF};
}

const uint32_t COLOR_BLACK = 0x000000;
const uint32_t COLOR_WHITE = 0xFFFFFF;
const uint32_t COLOR_RED = 0xFF0000;
const uint32_t COLOR_GREEN = 0x00FF00;
const uint32_t COLOR_BLUE = 0x0000FF;
const uint32_t COLOR_P1 = 0x00FF00; // Grün
const uint32_t COLOR_P2 = 0x0000FF; // Blau
const uint32_t COLOR_BALL = 0xFFFFFF; // Weiß
const uint32_t COLOR_LIFE_LOST = 0x400000; // Dunkelrot

led_strip_t strip;

// --- LED Strip Funktionen ---
void init_led_strip() {
    strip.type = LED_STRIP_WS2812;
    strip.length = NUM_LEDS;
    strip.gpio = LED_PIN;
    strip.buf = NULL; // Buffer wird von der Library alloziert
    strip.brightness = 100; // Helligkeit reduzieren, um Strom zu sparen und Augen zu schonen

    led_strip_install();
    ESP_ERROR_CHECK(led_strip_init(&strip));
    ESP_ERROR_CHECK(led_strip_clear(&strip)); // Alle LEDs ausschalten
    ESP_LOGI(TAG, "LED strip initialized.");
}

void set_pixel_color(int index, uint32_t color_val) {
    if (index >= 0 && index < NUM_LEDS) {
        led_strip_set_pixel(&strip, index, uint32ToRgb(color_val));
    }
}

void fill_color(uint32_t color_val) {
    for (int i = 0; i < NUM_LEDS; i++) {
        set_pixel_color(i, color_val);
    }
}

// --- Button Funktionen ---
void init_buttons() {
    button_p1.pin = BUTTON1_PIN;
    button_p2.pin = BUTTON2_PIN;
    Button *buttons[] = {&button_p1, &button_p2};

    for (int i = 0; i < 2; i++) {
        gpio_reset_pin(buttons[i]->pin);
        gpio_set_direction(buttons[i]->pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(buttons[i]->pin, GPIO_PULLUP_ONLY);
        buttons[i]->currentState = false;
        buttons[i]->lastState = false;
        buttons[i]->justPressed = false;
    }
    ESP_LOGI(TAG, "Buttons initialized.");
}

void update_button(Button *button) {
    button->lastState = button->currentState;
    button->currentState = !gpio_get_level(button->pin); // Active low wegen PULLUP
    button->justPressed = button->currentState && !button->lastState;
}

void process_input() {
    update_button(&button_p1);
    update_button(&button_p2);
}

// --- Spiel Initialisierung ---
void init_game_elements() {
    player1.side = LEFT;
    player1.lives = INITIAL_LIVES;
    player1.color = COLOR_P1;
    player1.paddle_pos_start = 0;
    player1.paddle_pos_end = PADDLE_SIZE -1;

    player2.side = RIGHT;
    player2.lives = INITIAL_LIVES;
    player2.color = COLOR_P2;
    player2.paddle_pos_start = NUM_LEDS - PADDLE_SIZE;
    player2.paddle_pos_end = NUM_LEDS -1;

    ball.color = COLOR_BALL;
    ball.speed = INITIAL_BALL_SPEED;

    servingPlayer = &player1; // Spieler 1 beginnt mit dem Aufschlag
    currentGameState = GAME_STATE_WAIT_SERVE;
    ESP_LOGI(TAG, "Game elements initialized. Player 1 serves.");
}

void prepare_serve() {
    ball.speed = INITIAL_BALL_SPEED;
    if (servingPlayer->side == LEFT) {
        ball.position = player1.paddle_pos_end + 1; // Knapp vor dem Schläger
        ball.direction = STOP; // Wartet auf Aktion
    } else {
        ball.position = player2.paddle_pos_start - 1;
        ball.direction = STOP;
    }
    currentGameState = GAME_STATE_WAIT_SERVE;
}

// --- Spiel Logik ---
void update_ball_position() {
    if (ball.direction == LEFT) {
        ball.position -= ball.speed;
    } else if (ball.direction == RIGHT) {
        ball.position += ball.speed;
    }

    // Kollision mit Wänden (Punktevergabe)
    if (ball.position < 0) {
        ESP_LOGI(TAG, "Ball out on left. Player 2 scores.");
        player1.lives--;
        servingPlayer = &player1; // Verlierer schlägt auf
        currentGameState = GAME_STATE_POINT_SCORED;
    } else if (ball.position >= NUM_LEDS) {
        ESP_LOGI(TAG, "Ball out on right. Player 1 scores.");
        player2.lives--;
        servingPlayer = &player2;
        currentGameState = GAME_STATE_POINT_SCORED;
    }

    // Kollision mit Schlägern
    int ball_led_idx = (int)ball.position;

    // Spieler 1 (links)
    if (ball.direction == LEFT && ball_led_idx <= player1.paddle_pos_end && ball_led_idx >= player1.paddle_pos_start) {
        if (button_p1.currentState) { // Schläger aktiv (Taste gehalten)
            ESP_LOGI(TAG, "Player 1 hit!");
            ball.direction = RIGHT;
            ball.position = player1.paddle_pos_end + 0.1f; // knapp außerhalb des Schlägers positionieren
            ball.speed += BALL_SPEED_INCREMENT;
        }
    }
    // Spieler 2 (rechts)
    else if (ball.direction == RIGHT && ball_led_idx >= player2.paddle_pos_start && ball_led_idx <= player2.paddle_pos_end) {
         if (button_p2.currentState) { // Schläger aktiv (Taste gehalten)
            ESP_LOGI(TAG, "Player 2 hit!");
            ball.direction = LEFT;
            ball.position = player2.paddle_pos_start - 0.1f;
            ball.speed += BALL_SPEED_INCREMENT;
        }
    }
}


void knightRiderAnimation(uint32_t color, int width, int repeats, int anim_speed_ms) {
    for (int r = 0; r < repeats; r++) {
        for (int i = 0; i <= NUM_LEDS - width; i++) {
            if (currentGameState != GAME_STATE_INIT && currentGameState != GAME_STATE_GAME_OVER) return; // Abbruch wenn Spielzustand sich ändert
            fill_color(COLOR_BLACK);
            for (int j = 0; j < width; j++) {
                set_pixel_color(i + j, color);
            }
            led_strip_flush(&strip);
            vTaskDelay(pdMS_TO_TICKS(anim_speed_ms));
        }
        for (int i = NUM_LEDS - width -1; i >= 0; i--) {
            if (currentGameState != GAME_STATE_INIT && currentGameState != GAME_STATE_GAME_OVER) return;
            fill_color(COLOR_BLACK);
            for (int j = 0; j < width; j++) {
                set_pixel_color(i + j, color);
            }
            led_strip_flush(&strip);
            vTaskDelay(pdMS_TO_TICKS(anim_speed_ms));
        }
    }
}

void game_update_logic() {
    static uint32_t last_ball_update_time = 0;
    uint32_t current_time = esp_log_timestamp(); // Gibt Millisekunden seit Boot

    switch (currentGameState) {
        case GAME_STATE_INIT:
            knightRiderAnimation(COLOR_RED, 5, 1, 70); // Startanimation
            init_game_elements(); // Setzt auch auf WAIT_SERVE
            // Fall through zu WAIT_SERVE, da init_game_elements den State setzt
            // oder explizit setzen: currentGameState = GAME_STATE_WAIT_SERVE;
            break;

        case GAME_STATE_WAIT_SERVE:
            // Ball steht still, wird in prepare_serve() positioniert
            if (servingPlayer == &player1 && button_p1.justPressed) {
                ball.direction = RIGHT;
                currentGameState = GAME_STATE_PLAYING;
                ESP_LOGI(TAG, "Player 1 serves right.");
            } else if (servingPlayer == &player2 && button_p2.justPressed) {
                ball.direction = LEFT;
                currentGameState = GAME_STATE_PLAYING;
                ESP_LOGI(TAG, "Player 2 serves left.");
            }
            break;

        case GAME_STATE_PLAYING:
            if ((current_time - last_ball_update_time) >= BALL_UPDATE_INTERVAL_MS) {
                update_ball_position();
                last_ball_update_time = current_time;
            }
            break;

        case GAME_STATE_POINT_SCORED:
            ESP_LOGI(TAG, "Point scored. P1 Lives: %d, P2 Lives: %d", player1.lives, player2.lives);
            // Kurze Pause oder Animation
            fill_color(COLOR_BLACK); // Alles aus
            if (servingPlayer == &player1) { // Player1 hat verloren, Player2 hat Punkt
                 for(int i=0; i<NUM_LEDS/2; i++) set_pixel_color(NUM_LEDS/2 + i, player2.color);
            } else { // Player2 hat verloren, Player1 hat Punkt
                 for(int i=0; i<NUM_LEDS/2; i++) set_pixel_color(i, player1.color);
            }
            led_strip_flush(&strip);
            vTaskDelay(pdMS_TO_TICKS(1000)); // 1 Sekunde Pause

            if (player1.lives == 0 || player2.lives == 0) {
                currentGameState = GAME_STATE_GAME_OVER;
            } else {
                prepare_serve(); // Bereitet nächsten Aufschlag vor
            }
            break;

        case GAME_STATE_GAME_OVER:
            ESP_LOGI(TAG, "Game Over!");
            uint32_t winner_color = (player1.lives > 0) ? player1.color : player2.color;
            for(int i=0; i<3; i++){ // Blinken lassen
                fill_color(winner_color);
                led_strip_flush(&strip);
                vTaskDelay(pdMS_TO_TICKS(300));
                fill_color(COLOR_BLACK);
                led_strip_flush(&strip);
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            // Warte auf einen Button-Druck für neues Spiel
            if (button_p1.justPressed || button_p2.justPressed) {
                 currentGameState = GAME_STATE_INIT; // Zurück zum Start
            }
            break;
    }
}

// --- Rendering ---
void render_paddles_and_lives() {
    // Spieler 1 Schläger und Leben
    for (int i = 0; i < PADDLE_SIZE; i++) {
        set_pixel_color(player1.paddle_pos_start + i, player1.color);
    }
    for (int i = 0; i < player1.lives; i++) {
         set_pixel_color(player1.paddle_pos_end + 1 + i, player1.color); // Leben direkt nach dem Schläger
    }
     for (int i = player1.lives; i < INITIAL_LIVES; i++) { // "Verlorene" Leben
         set_pixel_color(player1.paddle_pos_end + 1 + i, COLOR_LIFE_LOST);
    }


    // Spieler 2 Schläger und Leben
    for (int i = 0; i < PADDLE_SIZE; i++) {
        set_pixel_color(player2.paddle_pos_start + i, player2.color);
    }
    for (int i = 0; i < player2.lives; i++) {
        set_pixel_color(player2.paddle_pos_start - 1 - i, player2.color); // Leben direkt vor dem Schläger
    }
    for (int i = player2.lives; i < INITIAL_LIVES; i++) { // "Verlorene" Leben
        set_pixel_color(player2.paddle_pos_start - 1 - i, COLOR_LIFE_LOST);
    }
}

void render_ball() {
    if (currentGameState == GAME_STATE_PLAYING || currentGameState == GAME_STATE_WAIT_SERVE) {
        int ball_led_idx = (int)ball.position;
        if (ball_led_idx >=0 && ball_led_idx < NUM_LEDS) {
             set_pixel_color(ball_led_idx, ball.color);
        }
    }
}

void draw_game() {
    if (currentGameState != GAME_STATE_INIT && currentGameState != GAME_STATE_GAME_OVER && currentGameState != GAME_STATE_POINT_SCORED) {
        fill_color(COLOR_BLACK); // Hintergrund löschen
        render_paddles_and_lives();
        render_ball();
        led_strip_flush(&strip);
    }
    // Die anderen States (INIT, GAME_OVER, POINT_SCORED) handhaben ihr Rendering selbst
}

// --- Haupt-Task ---
void game_task(void *pvParameters) {
    ESP_LOGI(TAG, "Game task started.");
    init_led_strip();
    init_buttons();
    
    currentGameState = GAME_STATE_INIT; // Startzustand

    while (true) {
        process_input();
        game_update_logic();
        draw_game();
        vTaskDelay(pdMS_TO_TICKS(GAME_LOOP_DELAY_MS));
    }
}

void app_main(void) {
    esp_log_level_set(TAG, ESP_LOG_INFO);
    xTaskCreate(game_task, "game_task", 4096, NULL, 5, NULL);
}
