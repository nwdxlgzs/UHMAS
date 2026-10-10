#if defined(UHMAS_TEST_WITH_ADDRESS_SANITIZER)

#if defined(_MSC_VER)
#define UHMAS_SANITIZER_DEFAULT_OPTIONS_ATTR __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define UHMAS_SANITIZER_DEFAULT_OPTIONS_ATTR __attribute__((visibility("default"), used))
#else
#define UHMAS_SANITIZER_DEFAULT_OPTIONS_ATTR
#endif

UHMAS_SANITIZER_DEFAULT_OPTIONS_ATTR
const char *__asan_default_options(void) {
    return "detect_leaks=0:handle_segv=0:handle_sigbus=0:handle_sigill=0:"
           "handle_sigfpe=0:handle_abort=0";
}

UHMAS_SANITIZER_DEFAULT_OPTIONS_ATTR
const char *__ubsan_default_options(void) {
    return "print_stacktrace=1";
}

#endif
