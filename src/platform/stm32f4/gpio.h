


#define GPIO_A 0
#define GPIO_B 1
#define GPIO_C 2
#define GPIO_D 3
#define GPIO_E 4
#define GPIO_F 5
#define GPIO_G 6
#define GPIO_H 7
#define GPIO_I 8
#define GPIO_J 9
#define GPIO_K 10



typedef enum {
  GPIO_PULL_NONE = 0,
  GPIO_PULL_UP = 1,
  GPIO_PULL_DOWN = 2,
} gpio_pull_t;

typedef enum {
  GPIO_PUSH_PULL = 0,
  GPIO_OPEN_DRAIN = 1,
} gpio_output_type_t;


typedef enum {
  GPIO_SPEED_LOW       = 0,
  GPIO_SPEED_MID       = 1,
  GPIO_SPEED_HIGH      = 2,
  GPIO_SPEED_VERY_HIGH = 3,
} gpio_output_speed_t;




void gpio_conf_input(int port, int bit, gpio_pull_t pull);

void gpio_conf_output(int port, int bit, gpio_output_type_t type,
                      gpio_output_speed_t speed, gpio_pull_t pull);

void gpio_set_output(int port, int bit, int on);

void gpio_conf_af(int port, int bit, int af, gpio_output_speed_t speed,
                  gpio_pull_t pull);
