#include "check.h"

int check_failures;
int check_count;

int main(void)
{
    test_textbuf();
    test_input();
    test_proto();
    test_kbd();
    test_config();
    test_adpcm();
    printf("%d checks, %d failed\n", check_count, check_failures);
    return check_failures ? 1 : 0;
}
