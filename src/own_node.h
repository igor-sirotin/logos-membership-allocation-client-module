#pragma once

#ifdef __cplusplus
extern "C" {
#endif

char* allocation_node_call(const char* method, const char* args_json);
void allocation_node_free(char* reply);

#ifdef __cplusplus
}
#endif
