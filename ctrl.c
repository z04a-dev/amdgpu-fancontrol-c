// CUSTOM FAN CURVE FUNCTIONALITY FOR AMD GPUS
// BASED ON amdgpu-fancontrol BY grmat
// https://github.com/grmat/amdgpu-fancontrol
// I'VE ONLY REWRITTEN IT IN C (PRETTY BADLY) AND MEASURE AVERAGE TEMPERATURE BETWEEN GPU AND HOTSPOT
// BY z04a

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define FAIL_CODE -1

#define fan_max 0
#define fan_manual 1
#define fan_auto 2

const int HYSTERESIS = 6000;  // in mK
const int SLEEP_INTERVAL = 1; // in s

int temp_at_last_pwm_change;

// HARDCODED VALUES. DEBUG ONLY. REMOVE IN FINAL VERSION.
const int TEMPS[] = {30000, 45000, 55000, 67000, 75000};
const int PWMS[] = {56, 100, 125, 169, 255};
// HARDCODED

//HWMON IDENTIFIES THROUGH find_hwmon(), BUT CARD NUMBER IS STILL HARDCODED.
char *HWMON_PATH = "/sys/class/drm/card1/device/hwmon/hwmon"; 

char *FILE_PWM, *FILE_FANMODE, *FILE_TEMP, *FILE_JUNC;

bool file_exists(const char *filename) { return access(filename, F_OK) == 0; } //check if file exists

void find_ctrl_exception(char *err_msg) { //print error and exit
  printf("%s\n", err_msg);
  exit(FAIL_CODE);
}

void find_ctrl() { // check if all files are present
  char *_file_pwm, *_file_fanmode, *_file_temp, *_file_junc;
  // TEST IF PWM FILE AVAILABLE
  asprintf(&_file_pwm, "%s%s", HWMON_PATH, "pwm1");
  file_exists(_file_pwm) ? asprintf(&FILE_PWM, "%s", _file_pwm)
                         : find_ctrl_exception("PWM FILE NOT FOUND");

  // TEST IF FANMODE FILE AVAILABLE
  asprintf(&_file_fanmode, "%s%s", HWMON_PATH, "pwm1_enable");
  file_exists(_file_fanmode) ? asprintf(&FILE_FANMODE, "%s", _file_fanmode)
                             : find_ctrl_exception("FANMODE FILE NOT FOUND");

  // TEST IF TEMP FILE AVAILABLE
  asprintf(&_file_temp, "%s%s", HWMON_PATH, "temp1_input");
  file_exists(_file_temp) ? asprintf(&FILE_TEMP, "%s", _file_temp)
                          : find_ctrl_exception("JUNC FILE NOT FOUND");

  // TEST IF JUNCTION FILE AVAILABLE
  asprintf(&_file_junc, "%s%s", HWMON_PATH, "temp2_input");
  file_exists(_file_junc) ? asprintf(&FILE_JUNC, "%s", _file_junc)
                          : find_ctrl_exception("TEMP FILE NOT FOUND");

  free(_file_pwm);
  free(_file_fanmode);
  free(_file_temp);
  free(_file_junc);
}

void find_hwmon() { //find hwmon value
  bool _found = false;
  char *folder;
  for (int i = 0; i < 9; i++) {
    asprintf(&folder, "%s%d", HWMON_PATH, i);
    struct stat sb;
    if (stat(folder, &sb) == 0 && S_ISDIR(sb.st_mode)) {
      asprintf(&HWMON_PATH, "%s/", folder);
      _found = !_found;
      break;
    }
  }
  free(folder);
  if (!_found) {
    printf("%s\n", "unable to find hwmon? folder");
    exit(FAIL_CODE);
  }
  find_ctrl();
}

int values_equality() { // check if lengths of TEMPS[] and PWMS[] are equal
  int temps_length = sizeof(TEMPS) / sizeof(TEMPS[0]);
  int pwms_length = sizeof(PWMS) / sizeof(PWMS[0]);
  if (temps_length != pwms_length) {
    printf("%s\n", "Amount of temperature and pwm values does not match");
    exit(FAIL_CODE);
  }
  return pwms_length;
}

void check_for_sudo() { // speaks for itself
  uid_t uid = getuid(), euid = geteuid();
  if (!(uid == 0)) {
    printf("%s\n", "Writing to sysfs requires privileges, relaunch as root");
  }
}

void set_fanmode(int mode) { // conveniently change fan mode.
  char *command;
  printf("%s %d\n", "setting fan mode to ", mode);
  asprintf(&command, "echo %d > %s", mode, FILE_FANMODE);
  system(command);
  free(command);
}

int get_info(char *file_path) { // cat info from files.
  int result;
  char ch[1024];
  FILE *fptr;
  if ((fptr = fopen(file_path, "r")) == NULL) {
    printf("Error! Lost file in hwmon.");
    set_fanmode(fan_auto);
    exit(FAIL_CODE);
  }
  fgets(ch, sizeof ch, fptr);
  sscanf(ch, "%d", &result);
  fclose(fptr);
  return result;
}

void set_pwm(int new_pwm, int current_temp, int current_junc, int avg_temp,
             bool fan_change) { // set new pwm (or not)
  int old_pwm = get_info(FILE_PWM);

  printf("current pwm is: %d, requested pwm: %d\n", old_pwm, new_pwm);

  if (current_temp >= temp_at_last_pwm_change ||
      (avg_temp + HYSTERESIS) <= temp_at_last_pwm_change || fan_change) {
    printf("temp at last change was: %d\n", temp_at_last_pwm_change);
    printf("changing pwm to: %d\n", new_pwm);
    char *command;
    asprintf(&command, "echo %d > %s", new_pwm, FILE_PWM);
    system(command);
    free(command);
    temp_at_last_pwm_change = avg_temp;
  } else {
    printf("not changing pwm, we just did at %d, next change when below %d\n",
           temp_at_last_pwm_change, temp_at_last_pwm_change - HYSTERESIS);
  }
}

void interpolate_pwm(int entries_length) { // get new pwm from bizarre formula.
  int lower_temp, higher_temp, lower_pwm, higher_pwm, pwm;

  int current_temp = get_info(FILE_TEMP);
  int current_junc = get_info(FILE_JUNC);
  int current_fanmode = get_info(FILE_FANMODE);
  int avg_temp = (current_temp / 2) + (current_junc / 2);

  bool fan_change_happened = false;

  if (current_fanmode != 1) {
    set_fanmode(fan_manual);
    if (current_fanmode == 0) {
      fan_change_happened = !fan_change_happened;
    }
  }

  printf("current GPUtemp: %d\n", current_temp);
  printf("current junctemp: %d\n", current_junc);
  printf("current difftemp: %d\n", avg_temp);

  if (avg_temp <= TEMPS[0]) {
    set_pwm(PWMS[0], current_temp, current_junc, avg_temp, fan_change_happened);
    return;
  } else if (avg_temp >= TEMPS[entries_length - 1]) {
    set_pwm(PWMS[entries_length - 1], current_temp, current_junc, avg_temp,
            fan_change_happened);
    return;
  }

  for (int i = 0; i < entries_length; i++) {
    if (avg_temp >= TEMPS[i])
      continue;

    lower_temp = TEMPS[i - 1];
    higher_temp = TEMPS[i];
    lower_pwm = PWMS[i - 1];
    higher_pwm = PWMS[i];

    pwm = ((avg_temp - lower_temp) * (higher_pwm - lower_pwm) /
           (higher_temp - lower_temp)) +
          lower_pwm;
    printf("interpolated pwm value for temperature %d is: %d\n", avg_temp, pwm);

    set_pwm(pwm, current_temp, current_junc, avg_temp, fan_change_happened);
    return;
  }
}

void reset_on_exit() { // let fans control themselves automatically in case of program crash.
  printf("GOT SIGINT/SIGTERM. OR EXIT\n");
  set_fanmode(fan_auto);
  exit(0);
}

int main() {
  if (signal(SIGINT, reset_on_exit) == SIG_ERR) { // CATCHING SIGINT
    printf("\ncan't catch SIGINT\n");
  }

  if (signal(SIGTERM, reset_on_exit) == SIG_ERR) { // CATCHING SIGTERM
    printf("\ncan't catch SIGTERM\n");
  }

  check_for_sudo(); // check for sudo priveleges.
  find_hwmon();     // check if all files present.
  temp_at_last_pwm_change = get_info(FILE_PWM);
  set_fanmode(fan_manual);

  while (true) {
    system("clear");
    interpolate_pwm(values_equality());
    sleep(SLEEP_INTERVAL);
  }

  return 0;
}