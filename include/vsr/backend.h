#ifndef VSR_BACKEND_H
#define VSR_BACKEND_H

#include <stdbool.h>
#include <stddef.h>

// resolve an explicit or automatically detected hardware backend
bool vsr_backend_resolve(const char *requested, char *out, size_t out_len);

#endif // VSR_BACKEND_H
