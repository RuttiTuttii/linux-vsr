#include <stdio.h>

// forward declarations of test functions
void test_config_defaults(void);
void test_config_env_overrides(void);
void test_patcher_replace_all(void);
void test_patcher_combine_chunks(void);
void test_patcher_target_identification(void);
void test_patcher_injection(void);

int main(void) {
    printf("running linux-vsr unit test suite...\n");

    // execute config unit tests
    test_config_defaults();
    test_config_env_overrides();
    printf("  [pass] config module tests\n");

    // execute patcher unit tests
    test_patcher_replace_all();
    test_patcher_combine_chunks();
    test_patcher_target_identification();
    test_patcher_injection();
    printf("  [pass] patcher module tests\n");

    printf("all unit tests completed successfully\n");
    return 0;
}
