#include "ul212_rs232_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ul212ReadingZero(Ul212Reading *out)
{
  out->valid = false;
  out->height_mm = 0.0f;
  out->smooth_mm = 0.0f;
  out->temp_c = 0.0f;
  out->tilt_deg = 0;
  out->signal = 0;
  out->stamp_ms = 0;
}

static int ul212SplitCommaFields(const char *frame, char fields[][32], int max_fields)
{
  int count = 0;
  const char *start = frame;

  while (*start != '\0' && count < max_fields) {
    const char *end = strchr(start, ',');
    size_t len;

    if (end == NULL) {
      len = strlen(start);
      if (len >= sizeof(fields[0])) {
        return -1;
      }
      memcpy(fields[count], start, len);
      fields[count][len] = '\0';
      count++;
      break;
    }

    len = (size_t)(end - start);
    if (len >= sizeof(fields[0])) {
      return -1;
    }
    memcpy(fields[count], start, len);
    fields[count][len] = '\0';
    count++;
    start = end + 1;
  }

  return count;
}

bool ul212ParseXd(const char *frame, Ul212Reading *out)
{
  char fields[8][32];
  int field_count;
  const char *payload;
  char trailer[32];
  char *hash;
  int signal_raw;
  int temp_raw;
  char tilt_hex[3];

  if (frame == NULL || out == NULL) {
    return false;
  }

  ul212ReadingZero(out);

  if (strncmp(frame, "*XD,", 4) != 0) {
    return false;
  }

  payload = frame + 4;
  field_count = ul212SplitCommaFields(payload, fields, 7);
  if (field_count != 7) {
    return false;
  }

  if (strlen(fields[6]) < 2) {
    return false;
  }

  strncpy(trailer, fields[6], sizeof(trailer) - 1);
  trailer[sizeof(trailer) - 1] = '\0';
  hash = strchr(trailer, '#');
  if (hash == NULL) {
    return false;
  }
  *hash = '\0';

  if (strlen(trailer) < 2) {
    return false;
  }

  out->smooth_mm = (float)atoi(fields[2]) / 10.0f;
  out->height_mm = (float)atoi(fields[4]) / 10.0f;

  signal_raw = atoi(fields[3]);
  out->signal = (uint8_t)(signal_raw / 100);

  temp_raw = atoi(fields[5]);
  out->temp_c = (float)(temp_raw - 400) / 10.0f;

  tilt_hex[0] = trailer[strlen(trailer) - 2];
  tilt_hex[1] = trailer[strlen(trailer) - 1];
  tilt_hex[2] = '\0';
  out->tilt_deg = (uint8_t)strtol(tilt_hex, NULL, 16);

  out->valid = true;
  return true;
}

bool ul212ParseCfv(const char *frame, Ul212Reading *out)
{
  int addr;
  int raw_height;
  char height_digits[6];

  if (frame == NULL || out == NULL) {
    return false;
  }

  ul212ReadingZero(out);

  if (strncmp(frame, "*CFV", 4) != 0) {
    return false;
  }

  if (sscanf(frame + 4, "%2d%5s", &addr, height_digits) != 2) {
    return false;
  }

  raw_height = atoi(height_digits);
  out->height_mm = (float)raw_height / 10.0f;
  out->valid = true;
  return true;
}
