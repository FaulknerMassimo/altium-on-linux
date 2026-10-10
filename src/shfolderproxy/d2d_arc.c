/*
 * Direct2D arc segments as quadratic Bézier curves.
 *
 * Wine 11.0's d2d1 does not implement ID2D1GeometrySink::AddArc: it adds the end point
 * without a segment to it, so the arc is not drawn. This turns an arc, given as in
 * D2D1_ARC_SEGMENT (end point, radii, rotation, sweep direction, small or large), into the
 * curves that trace it, following the SVG implementation notes (endpoint to centre
 * parameterisation, radii scaled up when they are too small to reach the end point).
 * Quadratic curves, because Wine draws those exactly but replaces each cubic one with a
 * single quadratic.
 *
 * Freestanding C (no libc) so it builds into the Windows proxy DLL and, unchanged, into a
 * host test; the includer declares sin, cos, atan2 and sqrt.
 */

typedef struct { float x, y; } arc_point;                   /* D2D1_POINT_2F */
typedef struct { arc_point point1, point2; } arc_quadratic; /* D2D1_QUADRATIC_BEZIER_SEGMENT */

#define ARC_PI 3.14159265358979323846
#define ARC_STEP_MAX (ARC_PI / 6) /* 30 degrees per curve: at most 0.06% of the radius off the ellipse */
#define ARC_QUADRATICS_MAX 12

/* The curves along the arc from `from` to `to` (the last one ends exactly at `to`), with the
 * ellipse's x axis rotated by `rotation` degrees, clockwise (increasing angles, y pointing
 * down) or not, the large or the small one of the two possible arcs. Returns how many (at
 * most ARC_QUADRATICS_MAX), or 0 where the arc is a straight line (a zero radius, or the end
 * point is the start point and the line has no length). */
static unsigned arc_to_quadratics(arc_point from, arc_point to, float rx, float ry, float rotation,
                                  int clockwise, int large, arc_quadratic *out)
{
    double phi = rotation * (ARC_PI / 180), c = cos(phi), s = sin(phi);
    double dx = ((double)from.x - to.x) / 2, dy = ((double)from.y - to.y) / 2;
    double x1 = c * dx + s * dy, y1 = c * dy - s * dx; /* half the chord, in the ellipse's axes */
    double a = rx < 0 ? -rx : rx, b = ry < 0 ? -ry : ry;
    double lambda, k = 0, cx1, cy1, cx, cy, ux, uy, vx, vy, t, sweep, step, reach;
    unsigned n;

    if (!(a > 0 && b > 0 && a < 1e30 && b < 1e30) || (dx == 0 && dy == 0)) return 0;

    lambda = x1 * x1 / (a * a) + y1 * y1 / (b * b);
    if (!(lambda > 0 && lambda < 1e300)) return 0; /* also NaN or infinite points or rotation */
    if (lambda > 1) {
        a *= sqrt(lambda);
        b *= sqrt(lambda);
    } else {
        k = sqrt((1 - lambda) / lambda);
        if (large == clockwise) k = -k;
    }
    cx1 = k * a * y1 / b; /* the centre, relative to the chord's midpoint */
    cy1 = -k * b * x1 / a;
    cx = c * cx1 - s * cy1 + ((double)from.x + to.x) / 2;
    cy = s * cx1 + c * cy1 + ((double)from.y + to.y) / 2;

    /* the start and end points on the unit circle that the ellipse is mapped to */
    ux = (x1 - cx1) / a;
    uy = (y1 - cy1) / b;
    vx = (-x1 - cx1) / a;
    vy = (-y1 - cy1) / b;
    t = atan2(uy, ux);
    sweep = atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    if (clockwise && sweep < 0) sweep += 2 * ARC_PI;
    else if (!clockwise && sweep > 0) sweep -= 2 * ARC_PI;

    n = (unsigned)((sweep < 0 ? -sweep : sweep) / ARC_STEP_MAX - 1e-9) + 1;
    if (n > ARC_QUADRATICS_MAX) n = ARC_QUADRATICS_MAX;
    step = sweep / n;
    reach = 1 / cos(step / 2); /* where the tangents at both ends of a curve meet */

    for (unsigned i = 0; i < n; i++, t += step) {
        double mx = reach * cos(t + step / 2), my = reach * sin(t + step / 2);
        double ex = cos(t + step), ey = sin(t + step);
        out[i].point1.x = (float)(cx + a * c * mx - b * s * my);
        out[i].point1.y = (float)(cy + a * s * mx + b * c * my);
        out[i].point2.x = (float)(cx + a * c * ex - b * s * ey);
        out[i].point2.y = (float)(cy + a * s * ex + b * c * ey);
    }
    out[n - 1].point2 = to;
    return n;
}
