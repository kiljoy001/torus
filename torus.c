#include <u.h>
#include <libc.h>
#include <draw.h>
#include <event.h>
#include <bio.h>
#include <json.h>
#include "scene.h"

/*
 * torus - spin a torus at the rate of the local LLM, and bounce a shape
 * for each tool call.  oai appends requested tool names to $oaitoolfeed
 * (default /tmp/oai.toolcalls); see -f.
 */

enum {
	SH	= 1,		/* status line */
	FPS	= 30,
	Pollns	= 300000000LL,	/* 300 ms */
	NToolNames = 3,
};

enum {
	Idle,
	Prefill,
	Decode,
	Offline,
};

static Image *back;
static Image *green;
static Image *amber;
static Image *dim;
static Image *red;
static Image *toolcol;
static Font *f;
static int cw, ch;

static char *baseurl;
static char *feedpath;
static int state = Idle;
static char status[80] = "idle";
static double rate;
static int lastdecoded;
static vlong lastdecodedtime;
static vlong lastpoll;

static vlong feedoff;
static char feedpartial[96];
static int feedpartialn;
static char toolnames[NToolNames][16];
static int ntoolnames;

static int
fetch(char *base, char *out, int n)
{
	int cfd, m;
	char id[32], path[128];
	Biobuf *b;
	char *s;

	cfd = open("/mnt/web/clone", ORDWR);
	if(cfd < 0)
		return -1;
	m = read(cfd, id, sizeof id - 1);
	if(m <= 0){
		close(cfd);
		return -1;
	}
	id[m] = 0;
	m = atoi(id);

	fprint(cfd, "url %s/slots\n", base);
	snprint(path, sizeof path, "/mnt/web/%d/body", m);
	b = Bopen(path, OREAD);
	if(b == nil){
		close(cfd);
		return -1;
	}
	s = Brdstr(b, 0, 0);
	Bterm(b);
	close(cfd);
	if(s == nil)
		return -1;
	snprint(out, n, "%s", s);
	free(s);
	return 0;
}

static void
poll(void)
{
	char buf[8192];
	JSON *j, *slot, *nt, *v;
	JSONEl *el;
	int proc, dec, ptok;

	if(fetch(baseurl, buf, sizeof buf) < 0){
		state = Offline;
		snprint(status, sizeof status, "no server");
		return;
	}

	j = jsonparse(buf);
	if(j == nil || j->t != JSONArray){
		if(j != nil)
			jsonfree(j);
		state = Offline;
		snprint(status, sizeof status, "bad slots");
		return;
	}

	slot = nil;
	for(el = j->first; el != nil; el = el->next){
		v = jsonbyname(el->val, "is_processing");
		if(v != nil && v->t == JSONBool && v->n != 0){
			slot = el->val;
			break;
		}
	}
	if(slot == nil && j->first != nil)
		slot = j->first->val;

	v = slot != nil ? jsonbyname(slot, "is_processing") : nil;
	proc = v != nil && v->t == JSONBool && v->n != 0;

	if(!proc){
		state = Idle;
		rate = 0;
		lastdecoded = 0;
		lastdecodedtime = 0;
		snprint(status, sizeof status, "idle");
	} else {
		nt = jsonbyname(slot, "next_token");
		dec = 0;
		if(nt != nil && nt->t == JSONArray && nt->first != nil && nt->first->val != nil){
			v = jsonbyname(nt->first->val, "n_decoded");
			if(v != nil && v->t == JSONNumber)
				dec = v->n;
		}

		if(dec > 0){
			vlong now, dt;

			state = Decode;
			now = nsec();
			if(lastdecodedtime != 0 && dec > lastdecoded && now > lastdecodedtime){
				dt = now - lastdecodedtime;
				rate = (double)(dec - lastdecoded) * 1000000000.0 / (double)dt;
			}
			lastdecoded = dec;
			lastdecodedtime = now;
			snprint(status, sizeof status, "decode %.1f tok/s", rate);
		} else {
			state = Prefill;
			rate = 0;
			lastdecoded = 0;
			lastdecodedtime = 0;
			ptok = 0;
			v = jsonbyname(slot, "n_prompt_tokens");
			if(v != nil && v->t == JSONNumber)
				ptok = v->n;
			snprint(status, sizeof status, "prefill %d tok", ptok);
		}
	}
	jsonfree(j);
}

static void
remembertool(char *name)
{
	int i;

	for(i = 0; i < ntoolnames; i++){
		if(strcmp(toolnames[i], name) == 0){
			while(i > 0){
				strcpy(toolnames[i], toolnames[i - 1]);
				i--;
			}
			strncpy(toolnames[0], name, sizeof toolnames[0] - 1);
			toolnames[0][sizeof toolnames[0] - 1] = 0;
			return;
		}
	}
	if(ntoolnames < NToolNames)
		ntoolnames++;
	for(i = ntoolnames - 1; i > 0; i--)
		strcpy(toolnames[i], toolnames[i - 1]);
	strncpy(toolnames[0], name, sizeof toolnames[0] - 1);
	toolnames[0][sizeof toolnames[0] - 1] = 0;
}

static void
feedname(char *s)
{
	char name[16];
	int i;

	while(*s == ' ' || *s == '\t')
		s++;
	for(i = 0; i < (int)sizeof name - 1; i++){
		if(s[i] == 0 || s[i] == ' ' || s[i] == '\t'
		|| s[i] == '\n' || s[i] == '\r')
			break;
		name[i] = s[i];
	}
	name[i] = 0;
	if(i == 0)
		return;
	scenespawntool(name);
	remembertool(name);
}

static void
pollfeed(void)
{
	int fd, m, i;
	vlong n;
	char buf[512];

	if(feedpath == nil)
		return;
	fd = open(feedpath, OREAD);
	if(fd < 0)
		return;

	n = seek(fd, 0, 2);
	if(n < feedoff)
		feedoff = 0;
	if(n > feedoff && seek(fd, feedoff, 0) >= 0){
		while((m = read(fd, buf, sizeof buf)) > 0){
			feedoff += m;
			for(i = 0; i < m; i++){
				if(buf[i] == '\n'){
					feedpartial[feedpartialn] = 0;
					feedname(feedpartial);
					feedpartialn = 0;
				} else if(feedpartialn < (int)sizeof feedpartial - 1){
					feedpartial[feedpartialn++] = buf[i];
				}
			}
		}
	}
	close(fd);
}

static Image*
statecolor(void)
{
	switch(state){
	case Prefill:
		return amber;
	case Decode:
		return green;
	case Offline:
		return red;
	default:
		return dim;
	}
}

static void
setspeed(void)
{
	double s;

	switch(state){
	case Prefill:
		s = 0.10;
		break;
	case Decode:
		s = 0.02;
		if(rate > 0)
			s += rate / 200.0;
		if(s > 0.09)
			s = 0.09;
		break;
	default:
		s = 0.006;
		break;
	}
	scenesetspeed(s * FPS);
}

static Point
origin(void)
{
	Point p;

	p = screen->r.min;
	if(Dx(screen->r) > GW*cw)
		p.x += (Dx(screen->r) - GW*cw) / 2;
	if(Dy(screen->r) > (GH+SH)*ch)
		p.y += (Dy(screen->r) - (GH+SH)*ch) / 2;
	return p;
}

static void
redraw(void)
{
	char out[GH][GW+1];
	uchar style[GH][GW];
	char run[GW+1];
	char line[128];
	Point p;
	Image *fg;
	int x, y, i, n;

	scenerender(out, style);

	fg = statecolor();
	draw(back, back->r, display->black, nil, ZP);
	for(y = 0; y < GH; y++){
		for(x = 0; x < GW; ){
			i = x;
			while(i < GW && style[y][i] == style[y][x])
				i++;
			n = i - x;
			memmove(run, &out[y][x], n);
			run[n] = 0;
			string(back, Pt(x*cw, y*ch),
				style[y][x] == StyleTool ? toolcol : fg,
				ZP, f, run);
			x = i;
		}
	}

	n = snprint(line, sizeof line, "%s", status);
	if(ntoolnames > 0 && n < (int)sizeof line)
		n += snprint(line + n, sizeof line - n, " |");
	for(i = 0; i < ntoolnames && n < (int)sizeof line; i++)
		n += snprint(line + n, sizeof line - n, " %s", toolnames[i]);
	string(back, Pt(0, GH*ch), fg, ZP, f, line);

	p = origin();
	draw(screen, Rect(p.x, p.y, p.x + GW*cw, p.y + (GH+SH)*ch), back, nil, ZP);
	flushimage(display, 1);
}

void
eresized(int new)
{
	if(new && getwindow(display, Refnone) < 0)
		sysfatal("resize: %r");
	if(back)
		redraw();
}

static void
usage(void)
{
	fprint(2, "usage: %s [-t|-s] [-a] [-u baseurl] [-f toolfeed]\n", argv0);
	exits("usage");
}

static void
printframe(void)
{
	char out[GH][GW+1];
	uchar style[GH][GW];
	int y;

	scenerender(out, style);
	for(y = 0; y < GH; y++)
		print("%s\n", out[y]);
}

void
main(int argc, char **argv)
{
	Event e;
	ulong tk;
	int test, all, statusmode, i;

	test = 0;
	all = 0;
	statusmode = 0;
	baseurl = nil;
	feedpath = nil;

	ARGBEGIN{
	case 't':
		test = 1;
		break;
	case 'a':
		all = 1;
		break;
	case 's':
		statusmode = 1;
		break;
	case 'u':
		baseurl = EARGF(usage());
		break;
	case 'f':
		feedpath = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND;

	if(baseurl == nil)
		baseurl = getenv("oaiurl");
	if(baseurl == nil)
		baseurl = "http://127.0.0.1:8080";
	if(feedpath == nil)
		feedpath = getenv("oaitoolfeed");
	if(feedpath == nil)
		feedpath = "/tmp/oai.toolcalls";

	scenesrand(nsec());
	scenereset();

	if(test){
		if(all){
			scenespawntool("cmd");
			scenespawntool("read");
			scenespawntool("fetch");
			scenespawntool("agent");
			for(i = 0; i < 12; i++)
				sceneadvance(0.05);
		}
		printframe();
		exits(nil);
	}

	if(statusmode){
		for(i = 0; i < 20; i++){
			poll();
			pollfeed();
			print("%s\n", status);
			sleep(500);
		}
		exits(nil);
	}

	if(initdraw(nil, nil, "torus") < 0)
		sysfatal("initdraw: %r");

	f = openfont(display, "/lib/font/bit/fixed/unicode.6x13.font");
	if(f == nil)
		f = openfont(display, "/lib/font/bit/fixed/unicode.7x13.font");
	if(f == nil)
		sysfatal("openfont: %r");

	cw = stringwidth(f, "M");
	ch = f->height;

	green = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x33FF88FF);
	amber = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0xFFB000FF);
	dim = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x226644FF);
	red = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0xFF5555FF);
	toolcol = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x66CCFFFF);
	if(green == nil || amber == nil || dim == nil || red == nil || toolcol == nil)
		sysfatal("allocimage: %r");

	back = allocimage(display, Rect(0,0,GW*cw,(GH+SH)*ch), screen->chan, 0, DNofill);
	if(back == nil)
		sysfatal("allocimage: %r");

	einit(Emouse|Ekeyboard);
	tk = etimer(0, 1000/FPS);

	poll();
	pollfeed();
	setspeed();
	lastpoll = nsec();
	redraw();

	for(;;){
		switch(eread(Emouse|Ekeyboard|tk, &e)){
		case Ekeyboard:
			if(e.kbdc == 'q' || e.kbdc == 0x7f)
				exits(nil);
			break;
		case Emouse:
			break;
		default:
			if(nsec() - lastpoll > Pollns){
				poll();
				pollfeed();
				setspeed();
				lastpoll = nsec();
			}
			sceneadvance(1.0 / FPS);
			redraw();
			break;
		}
	}
}
