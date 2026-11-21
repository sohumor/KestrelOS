/* KestrelOS banner - print arguments as large 5x5 ASCII-art letters.
 *
 * usage: banner TEXT...
 *
 * Glyphs are 5 rows x 5 columns, drawn with '#'. Text wraps onto a new
 * band every WRAP characters so it always fits an 80-column console.
 * Lowercase is uppercased; characters with no glyph draw as '?'.
 */

#include <stdio.h>
#include <string.h>

#define GW 5                  /* glyph width */
#define GH 5                  /* glyph height */
#define WRAP 13               /* chars per band: 13 * (5+1) = 78 cols */

static const char *const G_A[] = {".###.", "#...#", "#####", "#...#", "#...#"};
static const char *const G_B[] = {"####.", "#...#", "####.", "#...#", "####."};
static const char *const G_C[] = {".####", "#....", "#....", "#....", ".####"};
static const char *const G_D[] = {"####.", "#...#", "#...#", "#...#", "####."};
static const char *const G_E[] = {"#####", "#....", "####.", "#....", "#####"};
static const char *const G_F[] = {"#####", "#....", "####.", "#....", "#...."};
static const char *const G_G[] = {".####", "#....", "#.###", "#...#", ".###."};
static const char *const G_H[] = {"#...#", "#...#", "#####", "#...#", "#...#"};
static const char *const G_I[] = {"#####", "..#..", "..#..", "..#..", "#####"};
static const char *const G_J[] = {".####", "...#.", "...#.", "#..#.", ".##.."};
static const char *const G_K[] = {"#...#", "#..#.", "###..", "#..#.", "#...#"};
static const char *const G_L[] = {"#....", "#....", "#....", "#....", "#####"};
static const char *const G_M[] = {"#...#", "##.##", "#.#.#", "#...#", "#...#"};
static const char *const G_N[] = {"#...#", "##..#", "#.#.#", "#..##", "#...#"};
static const char *const G_O[] = {".###.", "#...#", "#...#", "#...#", ".###."};
static const char *const G_P[] = {"####.", "#...#", "####.", "#....", "#...."};
static const char *const G_Q[] = {".###.", "#...#", "#.#.#", "#..#.", ".##.#"};
static const char *const G_R[] = {"####.", "#...#", "####.", "#..#.", "#...#"};
static const char *const G_S[] = {".####", "#....", ".###.", "....#", "####."};
static const char *const G_T[] = {"#####", "..#..", "..#..", "..#..", "..#.."};
static const char *const G_U[] = {"#...#", "#...#", "#...#", "#...#", ".###."};
static const char *const G_V[] = {"#...#", "#...#", "#...#", ".#.#.", "..#.."};
static const char *const G_W[] = {"#...#", "#...#", "#.#.#", "##.##", "#...#"};
static const char *const G_X[] = {"#...#", ".#.#.", "..#..", ".#.#.", "#...#"};
static const char *const G_Y[] = {"#...#", ".#.#.", "..#..", "..#..", "..#.."};
static const char *const G_Z[] = {"#####", "...#.", "..#..", ".#...", "#####"};

static const char *const G_0[] = {".###.", "#..##", "#.#.#", "##..#", ".###."};
static const char *const G_1[] = {"..#..", ".##..", "..#..", "..#..", ".###."};
static const char *const G_2[] = {".###.", "#...#", "..##.", ".#...", "#####"};
static const char *const G_3[] = {"####.", "....#", "..##.", "....#", "####."};
static const char *const G_4[] = {"...#.", "..##.", ".#.#.", "#####", "...#."};
static const char *const G_5[] = {"#####", "#....", "####.", "....#", "####."};
static const char *const G_6[] = {".###.", "#....", "####.", "#...#", ".###."};
static const char *const G_7[] = {"#####", "...#.", "..#..", ".#...", ".#..."};
static const char *const G_8[] = {".###.", "#...#", ".###.", "#...#", ".###."};
static const char *const G_9[] = {".###.", "#...#", ".####", "....#", ".###."};

static const char *const G_SP[]  = {"     ", "     ", "     ", "     ", "     "};
static const char *const G_DOT[] = {".....", ".....", ".....", ".....", "..#.."};
static const char *const G_COM[] = {".....", ".....", ".....", "..#..", ".#..."};
static const char *const G_EXC[] = {"..#..", "..#..", "..#..", ".....", "..#.."};
static const char *const G_QUE[] = {".###.", "#...#", "..##.", ".....", "..#.."};
static const char *const G_APOS[]= {"..#..", "..#..", ".....", ".....", "....."};
static const char *const G_MIN[] = {".....", ".....", ".###.", ".....", "....."};
static const char *const G_PLU[] = {".....", "..#..", ".###.", "..#..", "....."};
static const char *const G_SLA[] = {"....#", "...#.", "..#..", ".#...", "#...."};
static const char *const G_COL[] = {".....", "..#..", ".....", "..#..", "....."};
static const char *const G_LP[]  = {"...#.", "..#..", "..#..", "..#..", "...#."};
static const char *const G_RP[]  = {".#...", "..#..", "..#..", "..#..", ".#..."};
static const char *const G_EQ[]  = {".....", ".###.", ".....", ".###.", "....."};
static const char *const G_AST[] = {".....", ".#.#.", "..#..", ".#.#.", "....."};
static const char *const G_US[]  = {".....", ".....", ".....", ".....", "#####"};

static const char *const *glyph(int c)
{
    if (c >= 'a' && c <= 'z')
        c -= 'a' - 'A';
    switch (c) {
    case 'A': return G_A;  case 'B': return G_B;  case 'C': return G_C;
    case 'D': return G_D;  case 'E': return G_E;  case 'F': return G_F;
    case 'G': return G_G;  case 'H': return G_H;  case 'I': return G_I;
    case 'J': return G_J;  case 'K': return G_K;  case 'L': return G_L;
    case 'M': return G_M;  case 'N': return G_N;  case 'O': return G_O;
    case 'P': return G_P;  case 'Q': return G_Q;  case 'R': return G_R;
    case 'S': return G_S;  case 'T': return G_T;  case 'U': return G_U;
    case 'V': return G_V;  case 'W': return G_W;  case 'X': return G_X;
    case 'Y': return G_Y;  case 'Z': return G_Z;
    case '0': return G_0;  case '1': return G_1;  case '2': return G_2;
    case '3': return G_3;  case '4': return G_4;  case '5': return G_5;
    case '6': return G_6;  case '7': return G_7;  case '8': return G_8;
    case '9': return G_9;
    case ' ': return G_SP;
    case '.': return G_DOT;
    case ',': return G_COM;
    case '!': return G_EXC;
    case '?': return G_QUE;
    case '\'': return G_APOS;
    case '-': return G_MIN;
    case '+': return G_PLU;
    case '/': return G_SLA;
    case ':': return G_COL;
    case '(': return G_LP;
    case ')': return G_RP;
    case '=': return G_EQ;
    case '*': return G_AST;
    case '_': return G_US;
    default:  return G_QUE;
    }
}

/* Print one horizontal band of at most WRAP characters. */
static void print_band(const char *s, int n)
{
    int row, i;

    for (row = 0; row < GH; row++) {
        for (i = 0; i < n; i++) {
            const char *const *g = glyph((unsigned char)s[i]);
            printf("%s", g[row]);
            putchar(' ');
        }
        putchar('\n');
    }
}

int main(int argc, char **argv)
{
    static char msg[512];
    int i, len = 0, pos;

    /* Drop the shell-injected trailing "--cwd=<path>" argument. */
    if (argc > 1 && strncmp(argv[argc - 1], "--cwd=", 6) == 0)
        argc--;

    if (argc < 2) {
        printf("usage: banner TEXT...\n");
        return 1;
    }

    for (i = 1; i < argc && len < (int)sizeof(msg) - 1; i++) {
        const char *a = argv[i];
        if (i > 1)
            msg[len++] = ' ';
        while (*a && len < (int)sizeof(msg) - 1)
            msg[len++] = *a++;
    }
    msg[len] = '\0';

    for (pos = 0; pos < len; pos += WRAP) {
        int n = len - pos;
        if (n > WRAP)
            n = WRAP;
        if (pos)
            putchar('\n');
        print_band(msg + pos, n);
    }
    return 0;
}
