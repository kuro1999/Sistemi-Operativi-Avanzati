#ifndef SOA_DEMO_PROTOCOL_H
#define SOA_DEMO_PROTOCOL_H
#include <stdint.h>
#define DEMO_READY 1
#define DEMO_RESULT 2
struct demo_result {
    int kind;
    int error;
    int64_t value;
    uint64_t start_ns;
    uint64_t end_ns;
    uint32_t euid;
};
#endif
