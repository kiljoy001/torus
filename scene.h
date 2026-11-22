#ifndef HALATHA_SCENE_H
#define HALATHA_SCENE_H

enum {
	GW		= 60,	/* grid width in characters */
	GH		= 16,	/* grid height in characters */
	MaxShapes	= 8,
	StyleMain	= 0,
	StyleTool	= 1,
};

enum {
	STorus,
	SCube,
	SPyramid,
	STetra,
	SOcta,
	NShape,
};

typedef struct Shape Shape;
struct Shape {
	int	kind;
	double	x, y, z;	/* position in world space */
	double	vx, vy, vz;	/* bounce velocity */
	double	rx, ry, rz;	/* rotation */
	double	wx, wy, wz;	/* spin, radians/second */
	double	scale;
	double	life;		/* <= 0 means permanent */
	int	style;
	char	name[16];
};

void	scenesrand(ulong);
void	scenereset(void);
void	sceneclear(void);
Shape*	scenespawntool(char*);
void	scenesetspeed(double);
void	sceneadvance(double);
void	scenerender(char[GH][GW+1], uchar[GH][GW]);

#endif
