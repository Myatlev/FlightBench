/* Host test of the bench state's decisions (hw/arm/fb_bench_state.c.inc): the metadata's file
 * name, which sector + metadata a start restores (intact, damaged, another firmware's), and the
 * script's delta (the trainer's script + the GPS's lines, a changed and a removed line). The
 * files themselves: evidence/bench-state/ stage F on the machine. */
#include <stdio.h>
#include <stdlib.h>

#include "../hw/arm/fb_bench_state.c.inc"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void test_names(void)
{
    char b[64];
    CHECK(fbbs_json_name("/c/trainer.state.bin", b, sizeof(b)) && !strcmp(b, "/c/trainer.state.json"), "%s", b);
    CHECK(fbbs_json_name("C:\\Users\\k\\cfg\\trainer.state.BIN", b, sizeof(b)) &&
          !strcmp(b, "C:\\Users\\k\\cfg\\trainer.state.json"), "windows path: %s", b);
    CHECK(fbbs_json_name("bench-state", b, sizeof(b)) && !strcmp(b, "bench-state.json"), "no .bin: %s", b);
    CHECK(fbbs_json_name(".bin", b, sizeof(b)) && !strcmp(b, ".bin.json"), "only .bin: %s", b);
    CHECK(!fbbs_json_name("trainer.state.bin", b, 12), "too long for the buffer");
}

static void test_verdict(void)
{
    const char *sha = "b58288d8fd33", *fw = "3bfe4bef168e", *mach = "speedybee-f405-wing", *why;
    FbbsMeta ok = { true, 1, "speedybee-f405-wing", "3bfe4bef168e", "feature GPS\n", "b58288d8fd33" };
    FbbsMeta m;
    CHECK(fbbs_verdict(&ok, 131072, 131072, sha, mach, fw, &why) == FBBS_RESTORE && !why, "intact");
    CHECK(fbbs_verdict(&ok, 4096, 131072, sha, mach, fw, &why) == FBBS_SIZE, "truncated");
    CHECK(fbbs_verdict(&ok, 131073, 131072, sha, mach, fw, &why) == FBBS_SIZE, "too long");
    m = ok; m.parsed = false;
    CHECK(fbbs_verdict(&m, 131072, 131072, sha, mach, fw, &why) == FBBS_UNREADABLE && why, "not JSON");
    m = ok; m.format = 2;
    CHECK(fbbs_verdict(&m, 131072, 131072, sha, mach, fw, &why) == FBBS_NEWER && why, "format 2: a newer core's");
    m = ok; m.format = 2; m.firmware = "000000000000"; m.machine = NULL;
    CHECK(fbbs_verdict(&m, 131072, 131072, sha, mach, fw, &why) == FBBS_NEWER, "format 2 before its fields");
    m = ok; m.format = -1;
    CHECK(fbbs_verdict(&m, 131072, 131072, sha, mach, fw, &why) == FBBS_UNREADABLE, "format -1");
    m = ok; m.format = 0;
    CHECK(fbbs_verdict(&m, 131072, 131072, sha, mach, fw, &why) == FBBS_UNREADABLE, "no format");
    m = ok; m.machine = "nexus-xr";
    CHECK(fbbs_verdict(&m, 131072, 131072, sha, mach, fw, &why) == FBBS_UNREADABLE, "another machine");
    m = ok; m.machine = NULL;
    CHECK(fbbs_verdict(&m, 131072, 131072, sha, mach, fw, &why) == FBBS_UNREADABLE, "no machine");
    m = ok; m.script = NULL;
    CHECK(fbbs_verdict(&m, 131072, 131072, sha, mach, fw, &why) == FBBS_UNREADABLE, "no script");
    m = ok; m.script = "";
    CHECK(fbbs_verdict(&m, 131072, 131072, sha, mach, fw, &why) == FBBS_RESTORE, "an empty script is a script");
    m = ok; m.firmware = "000000000000";
    CHECK(fbbs_verdict(&m, 131072, 131072, sha, mach, fw, &why) == FBBS_FIRMWARE, "another firmware");
    m = ok; m.firmware = NULL;
    CHECK(fbbs_verdict(&m, 131072, 131072, sha, mach, fw, &why) == FBBS_FIRMWARE, "no firmware");
    CHECK(fbbs_verdict(&ok, 131072, 131072, sha, mach, NULL, &why) == FBBS_FIRMWARE, "this firmware unknown");
    m = ok; m.sector_sha256 = "0123";
    CHECK(fbbs_verdict(&m, 131072, 131072, sha, mach, fw, &why) == FBBS_RESTORE_OTHER_SHA, "another sector");
    m = ok; m.sector_sha256 = NULL;
    CHECK(fbbs_verdict(&m, 131072, 131072, sha, mach, fw, &why) == FBBS_RESTORE_OTHER_SHA, "no sector sha");
    m = ok; m.parsed = false; m.firmware = "x";
    CHECK(fbbs_verdict(&m, 4096, 131072, sha, mach, fw, &why) == FBBS_SIZE, "the size first");
}

static void test_delta(void)
{
    int add[16], rm[16], nrm;
    const char *had[] = { "set platform_type = AIRPLANE", "mixer_preset 14", "set nav_fw_launch_motor_delay = 500",
                          "aux 0 0 0 1700 2100" };
    /* the GPS wired: its four lines appended */
    const char *gps[] = { "set platform_type = AIRPLANE", "mixer_preset 14", "set nav_fw_launch_motor_delay = 500",
                          "aux 0 0 0 1700 2100", "feature GPS", "serial 2 +1", "set gps_provider = UBLOX",
                          "set gps_ublox_nav_hz = 10" };
    int n = fbbs_delta(gps, N(gps), had, N(had), add, rm, &nrm);
    CHECK(n == 4 && nrm == 0 && add[0] == 4 && add[3] == 7, "the GPS: %d added, %d removed", n, nrm);
    n = fbbs_delta(had, N(had), had, N(had), add, rm, &nrm);
    CHECK(n == 0 && nrm == 0, "unchanged: %d, %d", n, nrm);
    /* a changed value is sent (the new line) and the old one reported; a removed line reported */
    const char *chg[] = { "set platform_type = AIRPLANE", "mixer_preset 14", "set nav_fw_launch_motor_delay = 300" };
    n = fbbs_delta(chg, N(chg), had, N(had), add, rm, &nrm);
    CHECK(n == 1 && add[0] == 2 && nrm == 2 && rm[0] == 2 && rm[1] == 3, "changed + removed: %d, %d", n, nrm);
    /* the GPS unwired: its lines removed, nothing sent */
    n = fbbs_delta(had, N(had), gps, N(gps), add, NULL, &nrm);
    CHECK(n == 0 && nrm == 4, "the GPS unwired: %d, %d (removed counted without the array)", n, nrm);
    /* a first bench state from an empty script, and the other way */
    n = fbbs_delta(gps, N(gps), NULL, 0, add, rm, &nrm);
    CHECK(n == N(gps) && nrm == 0, "all new: %d", n);
    n = fbbs_delta(NULL, 0, had, N(had), add, rm, &nrm);
    CHECK(n == 0 && nrm == N(had), "all gone: %d", nrm);
    /* the order of the script is kept; a moved line is neither added nor removed */
    const char *mv[] = { "aux 0 0 0 1700 2100", "set platform_type = AIRPLANE", "mixer_preset 14",
                         "set nav_fw_launch_motor_delay = 500" };
    n = fbbs_delta(mv, N(mv), had, N(had), add, rm, &nrm);
    CHECK(n == 0 && nrm == 0, "moved: %d, %d", n, nrm);
}

int main(void)
{
    test_names();
    test_verdict();
    test_delta();
    printf("test_bench_state: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
