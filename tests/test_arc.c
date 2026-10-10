/* Host test driver for d2d_arc.c (run by tests/test_arc.py): for each input line
 * "from_x from_y to_x to_y rx ry rotation clockwise large", print the curve count and the
 * curves' points (control x y, end x y, ...). */
#include <math.h>
#include <stdio.h>
#include "d2d_arc.c"

int main(void)
{
    arc_point from, to;
    float rx, ry, rotation;
    int clockwise, large;
    arc_quadratic out[ARC_QUADRATICS_MAX];

    while (scanf("%f %f %f %f %f %f %f %d %d", &from.x, &from.y, &to.x, &to.y, &rx, &ry, &rotation,
                 &clockwise, &large) == 9) {
        unsigned n = arc_to_quadratics(from, to, rx, ry, rotation, clockwise, large, out);
        printf("%u", n);
        for (unsigned i = 0; i < n; i++)
            printf(" %.9g %.9g %.9g %.9g", out[i].point1.x, out[i].point1.y, out[i].point2.x, out[i].point2.y);
        putchar('\n');
    }
    return 0;
}
