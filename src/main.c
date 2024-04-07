#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rmt.h"
#include "led_strip.h"
#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "Game";

#define NUM_LEDS 54
#define LED_PIN GPIO_NUM_16
#define BUTTON1_PIN GPIO_NUM_25
#define BUTTON2_PIN GPIO_NUM_27
#define INITIAL_LIVES 5
#define INITIAL_SPEED 1
#define SPEEDUP 1
#define UPDATE_LOGIC_TIME 4

typedef enum { LEFT, RIGHT, STOP } side_type;

typedef struct {
    int pin;
    bool down;
    bool up;
    bool isPressed;
} Button;

typedef struct {
    uint32_t color;
    uint16_t lives;
    side_type side;
} Player;

typedef struct {
    uint32_t color;
    float position;
    side_type direction;
    float speed;
} Ball;

Button button1, button2;

Player p1, p2;
Ball ball;
bool quit = false;
bool start = false;
bool start_match = false;

rgb_t black = {.r = 0, .g = 0, .b = 0}; // black color
rgb_t white = {.r = 255, .g = 255, .b = 255}; // black color
rgb_t red = {.r = 255, .g = 0, .b = 0}; // Red color

rgb_t color_player1 = {.r = 0, .g = 255, .b = 0};
rgb_t color_player2 = {.r = 0, .g = 0, .b = 255};

rgb_t color_dead = {.r = 40, .g = 20, .b = 20};

led_strip_t strip = {
    .type = LED_STRIP_WS2812, // Set this according to your LED strip type
    .length = NUM_LEDS,
    .gpio = LED_PIN,
    //.channel = RMT_CHANNEL_0,
    .buf = NULL,
    .brightness = 255,
};

void init_buttons() {
    gpio_set_direction(BUTTON1_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON1_PIN, GPIO_PULLUP_ONLY);

    gpio_set_direction(BUTTON2_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON2_PIN, GPIO_PULLUP_ONLY);

    button1.pin = BUTTON1_PIN;
    button2.pin = BUTTON2_PIN;

    button1.up = true;
    button2.up = true;
}

void init_led_strip() {
    led_strip_install(); // Install the LED strip driver
    ESP_ERROR_CHECK(led_strip_init(&strip)); // Initialize the LED strip
}

void led_flush(led_strip_t *s){
    led_strip_flush(s);
    while(led_strip_busy(s));
}

void setAllTo(uint32_t color) {
    // Set all LEDs to a specific color
    for (int i = 0; i < NUM_LEDS; i++) {
        rgb_t pixel_color = {
            .r = (color >> 16) & 0xFF,
            .g = (color >> 8) & 0xFF,
            .b = color & 0xFF,
        };
        led_strip_set_pixel(&strip, i, pixel_color);
    }
    led_flush(&strip); // Update the strip with new colors
}

void processButtonInput(Button *button) {
    int currentState = gpio_get_level(button->pin); // Read the current button state
    bool pressed = !currentState;

    if (pressed != button->isPressed) { // State has changed
        if (pressed) {
            button->down = false; // Button was just pressed
            button->up = true;
        } else {
            button->up = false; // Button was just released
            button->down = true;
        }
        ESP_LOGI(TAG, "Button [%d] is %d %d %d", button->pin, button->isPressed, button->down, button->up);
    } else { // State is unchanged
        button->down = false;
        button->up = false;
    }

    button->isPressed = pressed;
}

void processInput() {
    processButtonInput(&button1);
    processButtonInput(&button2);
}

void startNewMatch(){
    ball.speed = INITIAL_SPEED;
    ball.position = NUM_LEDS / 2;
    ball.direction = STOP;
    start_match = true;
}

int but_num = 0;
void watchNewMatch(){

    if(start_match && (button1.isPressed || button2.isPressed) ){
        start_match = false;
        if(button1.isPressed){
            but_num = 1;
        }else{
            but_num = 2;
        }
    }
    if(but_num > 0){
        if( but_num == 1){ 
            if(!button1.isPressed){
                ball.direction = RIGHT;
                but_num = 0;
            }
        }else{
            if(!button2.isPressed){
                ball.direction = LEFT;
                but_num = 0;
            }
        }
    }
}

side_type changeDir(side_type dir){
    ball.speed = ball.speed+SPEEDUP;
    if(dir == LEFT) return RIGHT;
    else return LEFT;
}

void updateBall() {
    if(ball.direction != STOP){
    
        // Example ball movement logic
        if (ball.direction == LEFT) {
            ball.position -= ball.speed;
            if (ball.position < 0) {
                ball.position = 0;
                changeDir(ball.direction);
                p1.lives--; // Player 1 loses a life
                startNewMatch();
            }
        } else { // Ball moving right
            ball.position += ball.speed;
            if (ball.position > NUM_LEDS - 1) {
                ball.position = NUM_LEDS - 1;
                changeDir(ball.direction);
                p2.lives--; // Player 2 loses a life
                startNewMatch();
            }
        }
    }
}

void startGame(){

    if(start && (button1.isPressed || button2.isPressed) ){
        start = false;
        if(button1.isPressed){
            ball.direction = RIGHT;
        }else{
            ball.direction = LEFT;
        }
    }
}

void initStartGame(){
        ESP_LOGI(TAG, "start Game");
        p1.lives = INITIAL_LIVES;
        p2.lives = INITIAL_LIVES;
        ball.speed = INITIAL_SPEED;
        ball.position = NUM_LEDS / 2;
        ball.direction = STOP;
}

int inPlayerRange(Player* p){
    int score = -1;
    if(p->side == LEFT){
        if(ball.position <= INITIAL_LIVES){
            score = INITIAL_LIVES-ball.position;
        }
    }else{
        if(ball.position >= NUM_LEDS-INITIAL_LIVES){
            score = ball.position-(NUM_LEDS-INITIAL_LIVES);
        }
    }
    return score;
}


bool but1_state=false, but2_state=false;
void checkUserAction(){

    if(!start_match){
        int p1_score=0, p2_score=0;
        if(!but1_state && button1.isPressed){
            but1_state = true;
            p1_score = inPlayerRange(&p1);
            ESP_LOGI(TAG, "P1 score %d", p1_score);
            if(p1_score == -1){
                    p1.lives--; // Player 1 loses a life
                    startNewMatch();
            }else{
                ball.direction = changeDir(ball.direction);
                ball.speed = ball.speed+(int)(p1_score/2);
            }
        }else if(!button1.isPressed){
            but1_state = false;
        }
        if(!but2_state && button2.isPressed){
            but2_state = true;
            p2_score = inPlayerRange(&p2);
            ESP_LOGI(TAG, "P2 score %d", p2_score);
            if(p2_score == -1){
                    p2.lives--; // Player 2 loses a life
                    startNewMatch();
            }else{
                ball.direction = changeDir(ball.direction);
                ball.speed = ball.speed+(int)(p2_score/2);
            }
        }else if(!button2.isPressed){
            but2_state = false;
        }
    } 
}

bool knightRiderAnimation() {
    int animationSpeed = 70; // Adjust for desired animation speed, lower is faster
    int width = 5; // Width of the moving light
    int repeats = 1; // Number of times the animation repeats

    for (int r = 0; r < repeats; r++) {
        // Move light to the right
        for (int i = 0; i <= NUM_LEDS - width; i++) {
            led_strip_fill(&strip, 0, NUM_LEDS, black);
            for (int j = i; j < i + width; j++) {
                // Ensuring j is within the bounds of the LED strip
                if (j >= 0 && j < NUM_LEDS) {
                    led_strip_set_pixel(&strip, j, red);
                }
            }
            led_flush(&strip);
            vTaskDelay(pdMS_TO_TICKS(animationSpeed));
        }

        // Move light to the left
        for (int i = NUM_LEDS - width; i >= 0; i--) {
            led_strip_fill(&strip, 0, NUM_LEDS, black);
            for (int j = i; j < i + width; j++) {
                // Ensuring j is within the bounds of the LED strip
                if (j >= 0 && j < NUM_LEDS) {
                    led_strip_set_pixel(&strip, j, red);
                }
            }
            led_flush(&strip);
            vTaskDelay(pdMS_TO_TICKS(animationSpeed));
        }
    }
    return true;
}

void checkGameOver() {
    if (p1.lives <= 0 || p2.lives <= 0) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        knightRiderAnimation();
        initStartGame();
        start = true;
    }
}


int update_count = 0;
void updateGame() {
    processInput(); // Process input to potentially change the ball's direction
    checkUserAction();
    watchNewMatch();
    checkGameOver(); // Check for game over condition
    startGame();
    if(update_count >= UPDATE_LOGIC_TIME){
        update_count = 0;
        updateBall(); // Update the ball's position and handle bouncing
    }else{
        update_count++;
    }
}

void renderPlayer(Player* p) {
    int ledIndex;

    if (p->side == LEFT) {
        for (int i = 0; i < p->lives; i++) {
            ledIndex = i; // Starting from the left end for player 1
            led_strip_set_pixel(&strip, ledIndex, color_player1);
        }
        for (int i = p->lives; i < INITIAL_LIVES; i++) {
            ledIndex = i; // Starting from the left end for player 1
            led_strip_set_pixel(&strip, ledIndex, color_dead);
        }
    } else { // RIGHT
        for (int i = 0; i < p->lives; i++) {
            ledIndex = NUM_LEDS - 1 - i; // Starting from the right end for player 2
            led_strip_set_pixel(&strip, ledIndex, color_player2);
        }
        for (int i = p->lives; i < INITIAL_LIVES; i++) {
            ledIndex = NUM_LEDS - 1 - i; // Starting from the right end for player 2
            led_strip_set_pixel(&strip, ledIndex, color_dead);
        }
    }
}

void renderBall() {
    int ballIndex = (int)ball.position; // Convert ball's float position to an int for the LED index
    led_strip_set_pixel(&strip, ballIndex, white);
}

void drawGame() {
    led_strip_fill(&strip, 0, NUM_LEDS, black);

    renderPlayer(&p1); // Draw player 1
    renderPlayer(&p2); // Draw player 2
    renderBall(); // Draw the ball

    led_flush(&strip); // Update the strip to show the new LED states
}


void testLEDStrip() {
    for (int i = 0; i < NUM_LEDS; i++) {
        led_strip_fill(&strip, 0, NUM_LEDS, black);
        led_strip_set_pixel(&strip, i, white);
        led_flush(&strip);
        vTaskDelay(pdMS_TO_TICKS(50)); // Wait for a quarter second
    }
    led_strip_fill(&strip, 0, NUM_LEDS, black);
    led_flush(&strip);
}


void game_task(void *pvParameter) {
    ESP_LOGI(TAG, "Game task started.");
    //knightRiderAnimation();
    while (!quit) {
        processInput();
        updateGame();
        drawGame();
        vTaskDelay(pdMS_TO_TICKS(40)); // Adjust based on game speed requirements
    }
    ESP_LOGI(TAG, "Game task ending.");
}

void app_main() {
    init_led_strip();
    testLEDStrip();
    init_buttons();

    // Initialize players, ball, and game state
    p1.side = LEFT;
    p2.side = RIGHT;
    initStartGame();
    start = true;

    xTaskCreate(game_task, "game_task", 4096, NULL, 8, NULL);
}
