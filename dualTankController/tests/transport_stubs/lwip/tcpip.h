#pragma once
#include "dns.h"
inline err_t tcpip_try_callback(void (*callback)(void *), void *argument) {
  callback(argument);
  return ERR_OK;
}
