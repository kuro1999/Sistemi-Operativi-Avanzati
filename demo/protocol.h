#ifndef SOA_DEMO_PROTOCOL_H
#define SOA_DEMO_PROTOCOL_H
#include <stdint.h>
#define DEMO_READY 1
#define DEMO_RESULT 2
/* Codice riconoscibile: exit/exit_group devono terminare senza inviare RESULT. */
#define DEMO_EXIT_STATUS 42
/* Il caso O_TRUNC non deve mai puntare a un modulo realmente caricato. */
#define DEMO_ABSENT_MODULE "soa_demo_missing_target"
struct demo_result {
    int kind;
    int error;
    int64_t value;
    uint64_t start_ns;
    uint64_t end_ns;
    uint32_t euid;
};
#endif
