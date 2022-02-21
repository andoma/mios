#pragma once

static inline int isspace(int x)
{
  return x == ' ';
}

static inline int isdigit(int x)
{
  return x >= '0' && x <= '9';
}
