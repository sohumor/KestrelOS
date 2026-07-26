/* cal.c - print a month (or a whole year) as a calendar.
 *
 * Shipped in the kestrel-extras package rather than in /bin, so that
 * installing it is a real exercise of `kpkg install`.
 *
 * usage: cal                 this month, from the RTC
 *        cal <month>         that month of the current year
 *        cal <month> <year>
 *        cal -y [year]       twelve months
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLW 20            /* "Su Mo Tu We Th Fr Sa" */
#define ROWS 8             /* title, header, six week rows */

static const char *const month_name[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static const int month_days[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

/* Proleptic Gregorian, which is what the RTC pretends to be. */
static int leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int days_in_month(int m, int y)
{
    if (m == 2)
        return 28 + leap(y);
    return month_days[m - 1];
}

/* Day of week for a date, 0 = Sunday. Computed rather than read from
 * the clock so that any year can be printed. */
static int weekday(int y, int m, int d)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int yy = y;

    if (m < 3)
        yy--;
    return (yy + yy / 4 - yy / 100 + yy / 400 + t[m - 1] + d) % 7;
}

/* Render one month into ROWS blank-padded lines of COLW columns. Laying
 * it out this way lets -y print three months side by side. */
static void render(int m, int y, char rows[ROWS][COLW + 4], int with_year)
{
    int first = weekday(y, m, 1);
    int ndays = days_in_month(m, y);
    int day = 1, r, c, pad, len;
    char title[40];

    for (r = 0; r < ROWS; r++) {
        memset(rows[r], ' ', COLW);
        rows[r][COLW] = '\0';
    }

    if (with_year)
        snprintf(title, sizeof(title), "%s %d", month_name[m - 1], y);
    else
        snprintf(title, sizeof(title), "%s", month_name[m - 1]);
    len = (int)strlen(title);
    if (len > COLW)
        len = COLW;
    pad = (COLW - len) / 2;
    memcpy(rows[0] + pad, title, (unsigned long)len);
    memcpy(rows[1], "Su Mo Tu We Th Fr Sa", COLW);

    for (r = 2; r < ROWS; r++) {
        for (c = 0; c < 7; c++) {
            int slot = (r - 2) * 7 + c;
            char *cell = rows[r] + c * 3;
            if (slot < first || day > ndays)
                continue;
            cell[0] = day < 10 ? ' ' : (char)('0' + day / 10);
            cell[1] = (char)('0' + day % 10);
            day++;
        }
    }
}

static void put_trimmed(char *line, int n)
{
    while (n > 0 && line[n - 1] == ' ')
        n--;
    line[n] = '\0';
    printf("%s\n", line);
}

static void print_month(int m, int y)
{
    char rows[ROWS][COLW + 4];
    int r;

    render(m, y, rows, 1);
    for (r = 0; r < ROWS; r++)
        put_trimmed(rows[r], COLW);
}

static void print_year(int y)
{
    char rows[3][ROWS][COLW + 4];
    char line[3 * (COLW + 2) + 4];
    char head[32];
    int q, r, c, pos, len, pad;

    snprintf(head, sizeof(head), "%d", y);
    len = (int)strlen(head);
    pad = (3 * COLW + 4 - len) / 2;
    for (pos = 0; pos < pad; pos++)
        line[pos] = ' ';
    memcpy(line + pos, head, (unsigned long)len);
    line[pos + len] = '\0';
    printf("%s\n\n", line);

    for (q = 0; q < 4; q++) {
        for (c = 0; c < 3; c++)
            render(q * 3 + c + 1, y, rows[c], 0);
        for (r = 0; r < ROWS; r++) {
            pos = 0;
            for (c = 0; c < 3; c++) {
                memcpy(line + pos, rows[c][r], COLW);
                pos += COLW;
                if (c < 2) {
                    line[pos++] = ' ';
                    line[pos++] = ' ';
                }
            }
            put_trimmed(line, pos);
        }
        if (q < 3)
            printf("\n");
    }
}

/* Pull the shell-injected "--cwd=<path>" argument, wherever it sits. */
static int strip_cwd_arg(int argc, char **argv)
{
    int i, out = 1;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) != 0)
            argv[out++] = argv[i];
    }
    return out;
}

int main(int argc, char **argv)
{
    struct k_rtc tm;
    int nums[2], nnum = 0;
    int month, year, want_year = 0, i;

    argc = strip_cwd_arg(argc, argv);

    if (syscall(SYS_RTC, (long)&tm, 0, 0, 0) == 0 && tm.year >= 1 &&
        tm.mon >= 1 && tm.mon <= 12) {
        month = tm.mon;
        year = tm.year;
    } else {
        month = 1;
        year = 1970;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-y") == 0) {
            want_year = 1;
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: cal [-y] [month [year]]\n");
            return 0;
        } else if (argv[i][0] == '-') {
            printf("cal: unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            if (nnum >= 2) {
                printf("usage: cal [-y] [month [year]]\n");
                return 1;
            }
            nums[nnum++] = atoi(argv[i]);
        }
    }

    if (want_year) {
        if (nnum >= 1)
            year = nums[0];
    } else if (nnum == 1) {
        /* A single number is a month; a number over 12 means the year. */
        if (nums[0] >= 1 && nums[0] <= 12) {
            month = nums[0];
        } else {
            want_year = 1;
            year = nums[0];
        }
    } else if (nnum == 2) {
        month = nums[0];
        year = nums[1];
    }

    if (year < 1 || year > 9999) {
        printf("cal: year %d out of range (1-9999)\n", year);
        return 1;
    }
    if (want_year) {
        print_year(year);
        return 0;
    }
    if (month < 1 || month > 12) {
        printf("cal: month %d out of range (1-12)\n", month);
        return 1;
    }
    print_month(month, year);
    return 0;
}
