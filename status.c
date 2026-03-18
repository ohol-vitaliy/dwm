#include <X11/Xlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/wireless.h>
/* #include <stdarg.h> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Replace std::function lambdas with enums + stored paths */
typedef enum { CAP_FILE, CAP_ENERGY, CAP_CHARGE } CapacityType;
typedef enum { USAGE_POWER, USAGE_CURRENT } UsageType;

typedef struct {
  char name[64];
  char status[256];
  CapacityType cap_type;
  char cap_path1[256]; /* capacity / energy_now / charge_now */
  char cap_path2[256]; /* energy_full / charge_full           */
  UsageType usage_type;
  char usage_path1[256]; /* power_now / current_now */
  char usage_path2[256]; /* voltage_now             */
} BatteryPaths;

#define MAX_FREQ_PATHS 64
#define MAX_BATTERIES 16

static const char load_path[] = "/proc/loadavg";
static const char mem_path[] = "/proc/meminfo";
static const char net_path[] = "/sys/class/net";
static const char thermal_path[] = "/sys/class/thermal";

static int battery_count;
static int freq_path_count;
static Display *dpy;
static char iface[64];
static char value_bright_path[256];
static char max_bright_path[256];
static char temp_path[256];
static char freq_paths[MAX_FREQ_PATHS][256];
static BatteryPaths batteries[MAX_BATTERIES];

/* =====================================================================
 * Helpers
 * ===================================================================== */

static void combine_path(char *buf, size_t bufsz, const char *path,
                         const char *filename) {
  snprintf(buf, bufsz, "%s/%s", path, filename);
}

static int path_exists(const char *path) { return access(path, F_OK) == 0; }

static long read_number_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;

  char buf[32];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);

  if (n == 0)
    return -1;
  buf[n] = '\0';
  return strtol(buf, NULL, 10);
}

static void execscript(char *buf, size_t bufsz, const char *cmd) {
  buf[0] = '\0';
  FILE *pipe = popen(cmd, "r");
  if (!pipe)
    return;

  char tmp[2048];
  size_t pos = 0;
  while (fgets(tmp, sizeof(tmp), pipe) != NULL) {
    size_t len = strlen(tmp);
    if (pos + len < bufsz - 1) {
      memcpy(buf + pos, tmp, len);
      pos += len;
    }
  }
  buf[pos] = '\0';
  pclose(pipe);
}

/* =====================================================================
 * Load
 * ===================================================================== */

static void print_loadavg(char *buf, size_t bufsz) {
  buf[0] = '\0';
  FILE *f = fopen(load_path, "r");
  if (!f)
    return;

  double l1, l5, l15;
  char procs[32];
  fscanf(f, "%lf %lf %lf %31s", &l1, &l5, &l15, procs);
  fclose(f);
  snprintf(buf, bufsz, "%0.2f %0.2f %0.2f %s", l1, l5, l15, procs);
}

/* =====================================================================
 * CPU freq
 * ===================================================================== */

static void get_freq_paths(void) {
  glob_t glob_result;
  memset(&glob_result, 0, sizeof(glob_result));
  freq_path_count = 0;

  if (glob("/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq", 0, NULL,
           &glob_result) == 0) {
    for (size_t i = 0; i < glob_result.gl_pathc && i < MAX_FREQ_PATHS; ++i) {
      strncpy(freq_paths[i], glob_result.gl_pathv[i], 255);
      freq_paths[i][255] = '\0';
      ++freq_path_count;
    }
  }
  globfree(&glob_result);
}

static double print_cpu_freq(void) {
  if (freq_path_count == 0)
    return 0.0;

  double sum = 0.0;
  for (int i = 0; i < freq_path_count; ++i)
    sum += read_number_file(freq_paths[i]);

  return (sum / freq_path_count) / 1e6;
}

/* =====================================================================
 * RAM
 * ===================================================================== */

static void print_mem(char *buf, size_t bufsz) {
  buf[0] = '\0';
  FILE *f = fopen(mem_path, "r");
  if (!f)
    return;

  int found = 0;
  long total = 0, free_ = 0, buffers = 0, cached = 0, sreclaim = 0, shmem = 0;
  long value;
  char key[32], unit[8];

  while (fscanf(f, "%31s %ld %7s", key, &value, unit) == 3) {
    switch (key[0]) {
    case 'M':
      if (strncmp(key, "MemTotal:", 9) == 0) {
        total = value;
        ++found;
      } else if (strncmp(key, "MemFree:", 8) == 0) {
        free_ = value;
        ++found;
      }
      break;
    case 'B':
      if (strncmp(key, "Buffers:", 8) == 0) {
        buffers = value;
        ++found;
      }
      break;
    case 'C':
      if (strncmp(key, "Cached:", 7) == 0) {
        cached = value;
        ++found;
      }
      break;
    case 'S':
      if (strncmp(key, "SReclaimable:", 13) == 0) {
        sreclaim = value;
        ++found;
      } else if (strncmp(key, "Shmem:", 6) == 0) {
        shmem = value;
        ++found;
      }
      break;
    }
    if (found == 6)
      break;
  }
  fclose(f);

  const long avail = free_ + buffers + cached + sreclaim - shmem;
  const long used = total - avail;
  snprintf(buf, bufsz, "%0.2f GB/%0.2f GB", used / 1024.0 / 1024.0,
           avail / 1024.0 / 1024.0);
}

/* =====================================================================
 * Brightness
 * ===================================================================== */

static void get_brightness_dir(char *buf, size_t bufsz) {
  buf[0] = '\0';
  DIR *d = opendir("/sys/class/backlight");
  if (!d)
    return;

  struct dirent *e = readdir(d);
  while (e && e->d_name[0] == '.')
    e = readdir(d);

  if (e)
    snprintf(buf, bufsz, "/sys/class/backlight/%s", e->d_name);

  closedir(d);
}

static int print_brightness(void) {
  if (max_bright_path[0] == '\0' || value_bright_path[0] == '\0')
    return 0;

  const long max = read_number_file(max_bright_path);
  if (max <= 0)
    return 0;

  const long cur = read_number_file(value_bright_path);
  return (int)((cur * 100) / max);
}

/* =====================================================================
 * DateTime
 * ===================================================================== */

static void print_datetime(char *buf, size_t bufsz) {
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(buf, bufsz, "%a %F %R", &tm);
}

/* =====================================================================
 * Wi-Fi
 * ===================================================================== */

static void getSSID(char *buf, size_t bufsz, const char *iface_name) {
  buf[0] = '\0';
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
    return;

  struct iwreq req;
  memset(&req, 0, sizeof(req));
  strncpy(req.ifr_name, iface_name, IFNAMSIZ);

  char ssid[IW_ESSID_MAX_SIZE + 1];
  memset(ssid, 0, sizeof(ssid));
  req.u.essid.pointer = ssid;
  req.u.essid.length = IW_ESSID_MAX_SIZE + 1;
  req.u.essid.flags = 0;

  if (ioctl(sock, SIOCGIWESSID, &req) == -1) {
    close(sock);
    return;
  }
  close(sock);
  snprintf(buf, bufsz, "%.*s", (int)req.u.essid.length, ssid);
}

static void detect_wifi_iface(char *buf, size_t bufsz) {
  buf[0] = '\0';
  DIR *d = opendir(net_path);
  if (!d)
    return;

  struct dirent *e;
  while ((e = readdir(d))) {
    const char *n = e->d_name;
    if (!strncmp(n, "wlan", 4) || !strncmp(n, "wlp", 3) ||
        !strncmp(n, "wifi", 4)) {
      strncpy(buf, n, bufsz - 1);
      buf[bufsz - 1] = '\0';
      break;
    }
  }
  closedir(d);
}

/* =====================================================================
 * Temperature
 * ===================================================================== */

static void get_temp_path(void) {
  temp_path[0] = '\0';
  DIR *d = opendir(thermal_path);
  if (!d)
    return;

  struct dirent *e;
  while ((e = readdir(d))) {
    if (strncmp(e->d_name, "thermal_zone", 12) != 0)
      continue;

    char path[256];
    snprintf(path, sizeof(path), "/sys/class/thermal/%s/type", e->d_name);
    FILE *f = fopen(path, "r");
    if (!f)
      continue;

    char buf[32];
    fgets(buf, sizeof(buf), f);
    fclose(f);

    if (strstr(buf, "cpu") || strstr(buf, "x86_pkg_temp") ||
        strstr(buf, "package")) {
      snprintf(temp_path, sizeof(temp_path), "/sys/class/thermal/%s/temp",
               e->d_name);
      break;
    }
  }
  closedir(d);
}

static int get_temp(void) {
  const char *path = (temp_path[0] != '\0')
                         ? temp_path
                         : "/sys/class/thermal/thermal_zone0/temp";
  return (int)(read_number_file(path) / 1000);
}

/* =====================================================================
 * Battery
 * ===================================================================== */

static long battery_get_capacity(const BatteryPaths *b) {
  switch (b->cap_type) {
  case CAP_FILE:
    return read_number_file(b->cap_path1);
  case CAP_ENERGY:
  case CAP_CHARGE: {
    const long full = read_number_file(b->cap_path2);
    if (full > 0) {
      const long now = read_number_file(b->cap_path1);
      return now * 100 / full;
    }
    return 0;
  }
  }
  return 0;
}

static float battery_get_usage(const BatteryPaths *b) {
  if (b->usage_type == USAGE_POWER) {
    const long power = read_number_file(b->usage_path1);
    return power > 0 ? power / 1e6f : 0.0f;
  }
  const long cur = read_number_file(b->usage_path1);
  const long volt = read_number_file(b->usage_path2);
  return (cur * volt) / 1e12f;
}

static void detect_batteries(void) {
  DIR *d = opendir("/sys/class/power_supply");
  if (!d)
    return;

  battery_count = 0;
  struct dirent *e;

  while ((e = readdir(d)) && battery_count < MAX_BATTERIES) {
    if (e->d_name[0] == '.')
      continue;

    char type_path[256];
    snprintf(type_path, sizeof(type_path), "/sys/class/power_supply/%s/type",
             e->d_name);

    FILE *f = fopen(type_path, "r");
    if (!f)
      continue;

    char buf[32];
    fgets(buf, sizeof(buf), f);
    fclose(f);

    if (strncmp(buf, "Battery", 7) != 0)
      continue;

    BatteryPaths *b = &batteries[battery_count];
    memset(b, 0, sizeof(*b));

    char base[256];
    snprintf(base, sizeof(base), "/sys/class/power_supply/%s", e->d_name);
    combine_path(b->status, sizeof(b->status), base, "status");

    /* Capacity source */
    char capacity[256], energy_now[256], energy_full[256];
    combine_path(capacity, sizeof(capacity), base, "capacity");
    combine_path(energy_now, sizeof(energy_now), base, "energy_now");
    combine_path(energy_full, sizeof(energy_full), base, "energy_full");

    if (path_exists(capacity)) {
      b->cap_type = CAP_FILE;
      strncpy(b->cap_path1, capacity, sizeof(b->cap_path1) - 1);
    } else if (path_exists(energy_now)) {
      b->cap_type = CAP_ENERGY;
      strncpy(b->cap_path1, energy_now, sizeof(b->cap_path1) - 1);
      strncpy(b->cap_path2, energy_full, sizeof(b->cap_path2) - 1);
    } else {
      char charge_now[256], charge_full[256];
      combine_path(charge_now, sizeof(charge_now), base, "charge_now");
      combine_path(charge_full, sizeof(charge_full), base, "charge_full");
      b->cap_type = CAP_CHARGE;
      strncpy(b->cap_path1, charge_now, sizeof(b->cap_path1) - 1);
      strncpy(b->cap_path2, charge_full, sizeof(b->cap_path2) - 1);
    }

    /* Usage source */
    char power_now[256];
    combine_path(power_now, sizeof(power_now), base, "power_now");

    if (path_exists(power_now)) {
      b->usage_type = USAGE_POWER;
      strncpy(b->usage_path1, power_now, sizeof(b->usage_path1) - 1);
    } else {
      char current_now[256], voltage_now[256];
      combine_path(current_now, sizeof(current_now), base, "current_now");
      combine_path(voltage_now, sizeof(voltage_now), base, "voltage_now");
      b->usage_type = USAGE_CURRENT;
      strncpy(b->usage_path1, current_now, sizeof(b->usage_path1) - 1);
      strncpy(b->usage_path2, voltage_now, sizeof(b->usage_path2) - 1);
    }

    if (!strncmp(e->d_name, "BAT", 3))
      snprintf(b->name, sizeof(b->name), "B%s", e->d_name + 3);
    else
      strncpy(b->name, e->d_name, sizeof(b->name) - 1);

    ++battery_count;
  }
  closedir(d);
}

/* read_battery: replaces std::tuple return with output params */
static void read_battery(char *buf, size_t bufsz, int *charging,
                         const BatteryPaths *b) {
  *charging = 0;
  FILE *f = fopen(b->status, "r");
  if (f) {
    char sbuf[32];
    if (fgets(sbuf, sizeof(sbuf), f))
      *charging = !strncmp(sbuf, "Charging", 8);
    fclose(f);
  }

  const long capacity = battery_get_capacity(b);
  const float usage = battery_get_usage(b);
  snprintf(buf, bufsz, "%s: %ld%% (%0.2f W) ", b->name, capacity, usage);
}

static void print_battery_infos(char *buf, size_t bufsz) {
  buf[0] = '\0';
  int charging = 0;
  size_t pos = 0;

  for (int i = 0; i < battery_count; ++i) {
    char batt_data[128];
    int bat_ch = 0;
    read_battery(batt_data, sizeof(batt_data), &bat_ch, &batteries[i]);
    if (bat_ch)
      charging = 1;

    size_t len = strlen(batt_data);
    if (pos + len < bufsz - 1) {
      memcpy(buf + pos, batt_data, len);
      pos += len;
    }
  }

  if (charging && pos + 5 < bufsz) {
    memcpy(buf + pos, "CHRG ", 5);
    pos += 5;
  }
  buf[pos] = '\0';

  char tmp[32];
  snprintf(tmp, sizeof(tmp),
           "(%d\xc2\xb0"
           "C)",
           get_temp()); /* °C in UTF-8 */
  strncat(buf, tmp, bufsz - pos - 1);
}

/* =====================================================================
 * Loop
 * ===================================================================== */

void init_status() {
  detect_wifi_iface(iface, sizeof(iface));

  {
    char bright_dir[256];
    get_brightness_dir(bright_dir, sizeof(bright_dir));
    combine_path(max_bright_path, sizeof(max_bright_path), bright_dir,
                 "max_brightness");
    combine_path(value_bright_path, sizeof(value_bright_path), bright_dir,
                 "brightness");
  }

  get_temp_path();
  detect_batteries();
  get_freq_paths();
}

void get_topbar(char *buf, size_t bufsz) {
  char date[64] = {0};
  char kbmap[32] = {0};
  char mem[64] = {0};
  char ssid[64] = {0};
  char wifi[64] = {0};

  strncpy(wifi, "NC", sizeof(wifi) - 1);

  if (iface[0] != '\0') {
    getSSID(ssid, sizeof(ssid), iface);

    if (ssid[0] != '\0') {
      strncpy(wifi, ssid, sizeof(wifi) - 1);
    }
  }

  print_mem(mem, sizeof(mem));
  print_datetime(date, sizeof(date));
  execscript(kbmap, sizeof(kbmap),
             "setxkbmap -query | awk '/^layout/ {printf \"%s\", $2}'");

  snprintf(buf, bufsz, "| %s | %s | RAM: %s | %s |", wifi, kbmap, mem, date);
}

void get_extrabar_l(char *buf, size_t bufsz) {
  const int bright = print_brightness();
  snprintf(buf, bufsz, "| Bright: %d%% |", bright);
}

void get_extrabar_r(char *buf, size_t bufsz) {
  const double cpu_freq = print_cpu_freq();
  char load[64] = {0};
  char batt[256] = {0};

  print_loadavg(load, sizeof(load));
  print_battery_infos(batt, sizeof(batt));
  snprintf(buf, bufsz, "| %0.2f GHz | %s | %s |", cpu_freq, load, batt);
}

/* int main(void) */
/* { */
/*     if (!(dpy = XOpenDisplay(NULL))) */
/*     { */
/*         fprintf(stderr, "dwmstatus: cannot open display.\n"); */
/*         return 1; */
/*     } */

/*     int bright = 0; */
/*     double cpu_freq = 0.0; */
/*     char load[64] = {0}; */
/*     char mem[64] = {0}; */
/*     char date[64] = {0}; */
/*     char batt[256] = {0}; */
/*     char kbmap[32] = {0}; */
/*     char wifi[64] = {0}; */
/*     char ssid[64] = {0}; */
/*     char status[512] = {0}; */

/*     long counter = 0; */

/*     while (1) */
/*     { */
/*         ++counter; */

/*         if (counter >= 10) */
/*         { */
/*             counter = 0; */
/*             strncpy(wifi, "NC", sizeof(wifi) - 1); */

/*             if (iface[0] != '\0') */
/*             { */
/*                 getSSID(ssid, sizeof(ssid), iface); */
/*                 if (ssid[0] != '\0') */
/*                     strncpy(wifi, ssid, sizeof(wifi) - 1); */
/*             } */

/*             execscript( */
/*                 kbmap, */
/*                 sizeof(kbmap), */
/*                 "setxkbmap -query | awk '/^layout/ {printf \"%s\", $2}'"); */
/*             cpu_freq = print_cpu_freq(); */
/*         } */

/*         print_loadavg(load, sizeof(load)); */
/*         print_mem(mem, sizeof(mem)); */
/*         print_datetime(date, sizeof(date)); */
/*         bright = print_brightness(); */
/*         print_battery_infos(batt, sizeof(batt)); */

/*         snprintf( */
/*             status, */
/*             sizeof(status), */
/*             "| %s | %s | RAM: %s | %s |;| Bright: %d%% |;| %0.2f GHz | %s |
 * %s |", */
/*             wifi, */
/*             kbmap, */
/*             mem, */
/*             date, */
/*             bright, */
/*             cpu_freq, */
/*             load, */
/*             batt); */

/*         XStoreName(dpy, DefaultRootWindow(dpy), status); */
/*         XSync(dpy, False); */
/*         sleep(1); */
/*     } */

/*     XCloseDisplay(dpy); */
/*     return 0; */
/* } */
