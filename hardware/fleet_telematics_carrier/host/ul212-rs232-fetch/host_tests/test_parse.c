#include <stdio.h>
#include <math.h>
#include <string.h>
#include "ul212_rs232_parse.h"

static int fails;

static void expect_true(int cond, const char *msg) {
  if (!cond) { printf("FAIL: %s\n", msg); fails++; }
  else printf("OK: %s\n", msg);
}

int main(void) {
  Ul212Reading r;
  expect_true(ul212ParseXd("*XD,4850,20,0364,2300,0363,0651,1005#", &r), "parse xd");
  expect_true(r.valid, "xd valid");
  expect_true(fabsf(r.smooth_mm - 36.4f) < 0.05f, "smooth");
  expect_true(fabsf(r.height_mm - 36.3f) < 0.05f, "height");
  expect_true(fabsf(r.temp_c - 25.1f) < 0.05f, "temp");
  expect_true(r.signal == 23, "signal");
  expect_true(r.tilt_deg == 5, "tilt");

  memset(&r, 0, sizeof(r));
  expect_true(ul212ParseCfv("*CFV0100016CA4\r\n", &r), "parse cfv");
  expect_true(fabsf(r.height_mm - 1.6f) < 0.05f, "cfv height");

  expect_true(!ul212ParseXd("garbage", &r), "reject garbage");
  return fails ? 1 : 0;
}
