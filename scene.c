#include <u.h>
#include <libc.h>
#include "scene.h"

#define CAMZ	3.2
#define XSCALE	26.0
#define FILL	0.85	/* fraction of the limiting dimension to occupy */
#define PROJ	0.66	/* projected diameter per unit of world radius */

#define LX	-0.42
#define LY	-0.58
#define LZ	-0.70

static char ramp[] = " .:-=+*#%@";

typedef struct V3 V3;
struct V3 {
	double	x, y, z;
};

typedef struct Face Face;
struct Face {
	int	n;
	int	v[4];
};

typedef struct View View;
struct View {
	Shape	*s;
	double	scale;
	double	cx, sx;
	double	cy, sy;
	double	cz, sz;
};

typedef struct Canvas Canvas;
struct Canvas {
	char	(*out)[MaxGW+1];
	uchar	(*style)[MaxGW];
	double	(*zbuf)[MaxGW];
	int	w, h;
	double	sx, sy;
	double	zoom;
};

static Shape shape;
static int mainkind = STorus;
static double aspect = 2.0;
static double spin = 0.18;	/* radians/second */
static ulong rng = 1;

static V3 cubev[] = {
	{-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
	{-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1},
};

static Face cubef[] = {
	{4, {0, 1, 2, 3}},
	{4, {5, 4, 7, 6}},
	{4, {4, 0, 3, 7}},
	{4, {1, 5, 6, 2}},
	{4, {4, 5, 1, 0}},
	{4, {3, 2, 6, 7}},
};

static V3 pyramidv[] = {
	{ 0, -1.30,  0},
	{-1,  1.00, -1},
	{ 1,  1.00, -1},
	{ 1,  1.00,  1},
	{-1,  1.00,  1},
};

static Face pyramidf[] = {
	{3, {0, 1, 2}},
	{3, {0, 2, 3}},
	{3, {0, 3, 4}},
	{3, {0, 4, 1}},
	{4, {1, 4, 3, 2}},
};

static V3 tetrav[] = {
	{ 1,  1,  1},
	{ 1, -1, -1},
	{-1,  1, -1},
	{-1, -1,  1},
};

static Face tetraf[] = {
	{3, {0, 1, 2}},
	{3, {0, 3, 1}},
	{3, {0, 2, 3}},
	{3, {1, 3, 2}},
};

static V3 octav[] = {
	{ 1,  0,  0},
	{-1,  0,  0},
	{ 0,  1,  0},
	{ 0, -1,  0},
	{ 0,  0,  1},
	{ 0,  0, -1},
};

static Face octaf[] = {
	{3, {0, 2, 4}}, {3, {0, 4, 3}}, {3, {0, 3, 5}}, {3, {0, 5, 2}},
	{3, {1, 4, 2}}, {3, {1, 3, 4}}, {3, {1, 5, 3}}, {3, {1, 2, 5}},
};

static double
scenerand(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (double)(rng & 0xffffff) / 16777215.0;
}

void
scenesrand(ulong seed)
{
	rng = seed;
	if(rng == 0)
		rng = 0x9e3779b9;
}

void
scenesetspeed(double s)
{
	if(s < 0)
		s = 0;
	if(s > 6)
		s = 6;
	spin = s;
}

static double
shaperadius(int kind)
{
	switch(kind){
	case STorus:
		return 1.36;
	case SOcta:
		return 1.0;
	default:
		return 1.7320508075688772;	/* cube corner, square pyramid base, tetra vertex */
	}
}

int
scenesetshape(char *name)
{
	if(strcmp(name, "torus") == 0)
		mainkind = STorus;
	else if(strcmp(name, "cube") == 0)
		mainkind = SCube;
	else if(strcmp(name, "pyramid") == 0)
		mainkind = SPyramid;
	else if(strcmp(name, "tetra") == 0)
		mainkind = STetra;
	else if(strcmp(name, "octa") == 0)
		mainkind = SOcta;
	else
		return -1;
	return 0;
}

void
scenesetaspect(double a)
{
	/* character cells are taller than they are wide */
	if(a > 0.5 && a < 8.0)
		aspect = a;
}

void
scenereset(void)
{
	memset(&shape, 0, sizeof shape);
	shape.kind = mainkind;
	shape.scale = 1.0 / shaperadius(mainkind);
	shape.style = StyleMain;
	shape.rx = 0.35 + (scenerand() - 0.5) * 0.10;
	shape.ry = 0.15 + (scenerand() - 0.5) * 0.10;
	shape.rz = (scenerand() - 0.5) * 0.10;
}

void
sceneadvance(double dt)
{
	if(dt <= 0)
		return;
	if(dt > 0.25)
		dt = 0.25;

	shape.rx += spin * dt;
	shape.ry += spin * 0.6 * dt;
	shape.rz += spin * 0.3 * dt;
}

static void
viewinit(View *v, Shape *s)
{
	v->s = s;
	v->scale = s->scale;
	v->cx = cos(s->rx);
	v->sx = sin(s->rx);
	v->cy = cos(s->ry);
	v->sy = sin(s->ry);
	v->cz = cos(s->rz);
	v->sz = sin(s->rz);
}

static void
viewpoint(View *v, double x, double y, double z, double *wx, double *wy, double *wz)
{
	double y1, z1, x2, z2, x3, y3;
	Shape *s;

	s = v->s;

	y1 = y * v->cx - z * v->sx;
	z1 = y * v->sx + z * v->cx;
	x2 = x * v->cy + z1 * v->sy;
	z2 = -x * v->sy + z1 * v->cy;
	x3 = x2 * v->cz - y1 * v->sz;
	y3 = x2 * v->sz + y1 * v->cz;

	*wx = x3 * v->scale + s->x;
	*wy = y3 * v->scale + s->y;
	*wz = z2 * v->scale + s->z;
}

static void
viewnormal(View *v, double x, double y, double z, double *nx, double *ny, double *nz)
{
	double y1, z1, x2;

	y1 = y * v->cx - z * v->sx;
	z1 = y * v->sx + z * v->cx;
	x2 = x * v->cy + z1 * v->sy;
	*nz = -x * v->sy + z1 * v->cy;
	*nx = x2 * v->cz - y1 * v->sz;
	*ny = x2 * v->sz + y1 * v->cz;
}

static int
shadeof(View *v, double nx, double ny, double nz)
{
	double lum, rn[3];
	int idx;

	viewnormal(v, nx, ny, nz, &rn[0], &rn[1], &rn[2]);
	lum = rn[0] * LX + rn[1] * LY + rn[2] * LZ;
	if(lum < 0)
		lum = 0;
	lum = 0.15 + 0.85 * lum;
	idx = 1 + (int)(lum * 8.0);
	if(idx > 9)
		idx = 9;
	return idx;
}

static void
plot(View *v, Canvas *c, double x, double y, double z, char ch, double bias)
{
	double d;
	int px, py;

	if(z + CAMZ < 0.35)
		return;
	d = 1.0 / (z + CAMZ);
	px = c->w / 2 + (int)(c->sx * x * d);
	py = c->h / 2 + (int)(c->sy * y * d);
	if(px < 0 || px >= c->w || py < 0 || py >= c->h)
		return;
	if(d + bias < c->zbuf[py][px])
		return;
	c->zbuf[py][px] = d;
	c->out[py][px] = ch;
	c->style[py][px] = v->s->style;
}

static void
edge(View *v, Canvas *c, V3 a, V3 b)
{
	double dx, dy, dz, len, t, x, y, z;
	int i, n;

	dx = b.x - a.x;
	dy = b.y - a.y;
	dz = b.z - a.z;
	len = sqrt(dx * dx + dy * dy + dz * dz);
	n = (int)((3 + len * 14.0) * c->zoom);
	if(n < 4)
		n = 4;
	if(n > 240)
		n = 240;

	for(i = 0; i <= n; i++){
		t = (double)i / (double)n;
		viewpoint(v, a.x + dx * t, a.y + dy * t, a.z + dz * t, &x, &y, &z);
		plot(v, c, x, y, z, '@', 0.004);
	}
}

static void
face(View *v, Canvas *c, V3 *verts, Face *f)
{
	V3 a, b, cen, p;
	double u, w, nx, ny, nz, len, x, y, z, step;
	int i, idx;

	nx = ny = nz = 0;
	cen.x = cen.y = cen.z = 0;
	for(i = 0; i < f->n; i++){
		a = verts[f->v[i]];
		b = verts[f->v[(i + 1) % f->n]];
		nx += (a.y - b.y) * (a.z + b.z);
		ny += (a.z - b.z) * (a.x + b.x);
		nz += (a.x - b.x) * (a.y + b.y);
		cen.x += a.x;
		cen.y += a.y;
		cen.z += a.z;
	}
	cen.x /= f->n;
	cen.y /= f->n;
	cen.z /= f->n;

	len = sqrt(nx * nx + ny * ny + nz * nz);
	if(len < 1e-9)
		return;
	nx /= len;
	ny /= len;
	nz /= len;
	if(nx * cen.x + ny * cen.y + nz * cen.z < 0){
		nx = -nx;
		ny = -ny;
		nz = -nz;
	}

	idx = shadeof(v, nx, ny, nz);

	step = 0.055 / c->zoom;
	if(step < 0.012)
		step = 0.012;
	if(step > 0.12)
		step = 0.12;

	for(i = 1; i < f->n - 1; i++){
		for(u = 0; u <= 1.0; u += step){
			for(w = 0; w <= 1.0 - u; w += step){
				p.x = verts[f->v[0]].x
					+ u * (verts[f->v[i]].x - verts[f->v[0]].x)
					+ w * (verts[f->v[i + 1]].x - verts[f->v[0]].x);
				p.y = verts[f->v[0]].y
					+ u * (verts[f->v[i]].y - verts[f->v[0]].y)
					+ w * (verts[f->v[i + 1]].y - verts[f->v[0]].y);
				p.z = verts[f->v[0]].z
					+ u * (verts[f->v[i]].z - verts[f->v[0]].z)
					+ w * (verts[f->v[i + 1]].z - verts[f->v[0]].z);
				viewpoint(v, p.x, p.y, p.z, &x, &y, &z);
				plot(v, c, x, y, z, ramp[idx], 0.0);
			}
		}
	}

	for(i = 0; i < f->n; i++)
		edge(v, c, verts[f->v[i]], verts[f->v[(i + 1) % f->n]]);
}

static void
poly(View *v, Canvas *c, V3 *verts, Face *faces, int nface)
{
	int i;

	for(i = 0; i < nface; i++)
		face(v, c, verts, &faces[i]);
}

static void
torus(View *v, Canvas *c)
{
	double u, w, cu, su, cw, sw, r, x, y, z, nx, ny, nz;
	double du, dw;
	int idx;

	du = 0.09 / c->zoom;
	dw = 0.13 / c->zoom;
	if(du < 0.02)
		du = 0.02;
	if(du > 0.18)
		du = 0.18;
	if(dw < 0.03)
		dw = 0.03;
	if(dw > 0.26)
		dw = 0.26;

	for(u = 0; u < 6.283185307179586; u += du){
		cu = cos(u);
		su = sin(u);
		for(w = 0; w < 6.283185307179586; w += dw){
			cw = cos(w);
			sw = sin(w);
			r = 0.36;
			x = (1.0 + r * cw) * cu;
			y = (1.0 + r * cw) * su;
			z = r * sw;
			nx = cw * cu;
			ny = cw * su;
			nz = sw;
			idx = shadeof(v, nx, ny, nz);
			viewpoint(v, x, y, z, &x, &y, &z);
			plot(v, c, x, y, z, ramp[idx], 0.0);
		}
	}
}

void
scenerender(char out[MaxGH][MaxGW+1], uchar style[MaxGH][MaxGW], int w, int h)
{
	static double zbuf[MaxGH][MaxGW];
	Canvas c;
	View v;
	double fit;
	int x, y;

	if(w > MaxGW)
		w = MaxGW;
	if(h > MaxGH)
		h = MaxGH;
	if(w < 1)
		w = 1;
	if(h < 1)
		h = 1;

	c.out = out;
	c.style = style;
	c.zbuf = zbuf;
	c.w = w;
	c.h = h;

	/*
	 * Fill most of the limiting dimension.  The projection is
	 * circular in pixels when sx/sy equals the cell aspect ratio
	 * (height/width), so derive sy from sx and scale both from
	 * the smaller of the available width and the width equivalent
	 * of the height.
	 */
	fit = w;
	if(h * aspect < fit)
		fit = h * aspect;
	c.zoom = FILL * fit / (PROJ * XSCALE);
	if(c.zoom < 0.35)
		c.zoom = 0.35;
	c.sx = XSCALE * c.zoom;
	c.sy = c.sx / aspect;

	for(y = 0; y < h; y++){
		for(x = 0; x < w; x++){
			out[y][x] = ' ';
			style[y][x] = StyleMain;
			zbuf[y][x] = 0;
		}
		out[y][w] = 0;
	}

	viewinit(&v, &shape);
	switch(shape.kind){
	case STorus:
		torus(&v, &c);
		break;
	case SCube:
		poly(&v, &c, cubev, cubef, nelem(cubef));
		break;
	case SPyramid:
		poly(&v, &c, pyramidv, pyramidf, nelem(pyramidf));
		break;
	case STetra:
		poly(&v, &c, tetrav, tetraf, nelem(tetraf));
		break;
	case SOcta:
		poly(&v, &c, octav, octaf, nelem(octaf));
		break;
	}
}
