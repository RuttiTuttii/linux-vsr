#include <stdio.h>

// forward declarations of test functions
void test_config_defaults(void);
void test_config_env_overrides(void);
void test_config_invalid_env(void);
void test_config_file_roundtrip(void);
void test_config_validate(void);
void test_patcher_replace_all(void);
void test_patcher_replace_safety(void);
void test_patcher_combine_chunks(void);
void test_patcher_combine_safety(void);
void test_patcher_target_identification(void);
void test_patcher_injection(void);
void test_patcher_injection_safety(void);
void test_safety_alloc(void);
void test_safety_strings(void);
void test_safety_math(void);
void test_safety_float(void);
void test_safety_error_slot(void);
void test_shaders_cas_basic(void);
void test_shaders_cas_watermark(void);
void test_shaders_easu(void);
void test_shaders_dispatch(void);

int main(void) {
    // announce test suite start
    printf("running linux-vsr unit test suite...\n");
    // execute config unit tests
    test_config_defaults();
    test_config_env_overrides();
    test_config_invalid_env();
    test_config_file_roundtrip();
    test_config_validate();
    printf("  [pass] config module tests\n");
    // execute patcher unit tests
    test_patcher_replace_all();
    test_patcher_replace_safety();
    test_patcher_combine_chunks();
    test_patcher_combine_safety();
    test_patcher_target_identification();
    test_patcher_injection();
    test_patcher_injection_safety();
    printf("  [pass] patcher module tests\n");
    // execute safety unit tests
    test_safety_alloc();
    test_safety_strings();
    test_safety_math();
    test_safety_float();
    test_safety_error_slot();
    printf("  [pass] safety module tests\n");
    // execute shader unit tests
    test_shaders_cas_basic();
    test_shaders_cas_watermark();
    test_shaders_easu();
    test_shaders_dispatch();
    printf("  [pass] shaders module tests\n");
    // report success
    printf("all unit tests completed successfully\n");
    return 0;
}
