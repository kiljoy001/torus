#ifndef HALATHA_SCENE_H
#define HALATHA_SCENE_H

enum {
	GW		= 60,	/* base grid width in characters */
	GH		= 16,	/* base grid height in characters */
	MaxGW		= 600,	/* largest grid we will render */
	MaxGH		= 160,
	StyleMain	= 0,
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
	double	rx, ry, rz;	/* rotation */
	double	scale;
	int	style;
	char	name[16];
};

void	scenesrand(ulong);
void	scenereset(void);
int	scenesetshape(char*);
void	scenesetaspect(double);
void	scenesetspeed(double);
void	sceneadvance(double);
void	scenerender(char[MaxGH][MaxGW+1], uchar[MaxGH][MaxGW], int, int);

#endif
