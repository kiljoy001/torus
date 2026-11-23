#include <u.h>
#include <libc.h>
#include <draw.h>
#include <event.h>
#include <bio.h>
#include <json.h>
#include <thread.h>
#include "scene.h"

/*
 * torus - spin a shape at the rate of the local LLM and paint the work
 * the harness is doing across it in colour waves.  oai appends actions
 * to $oaitoolfeed (default /tmp/oai.toolcalls); see -f.
 *
 * Feed lines are one of:
 *
 *	name			legacy; state=start, class guessed from name
 *	name state		state is start, end, ok, error, ...
 *	kind name state		kind is cmd, read, write, net, agent, mem,
 *				view, or tool
 *
 * Whitespace separates fields.  A bare name keeps working.
 */

enum {
	SH		= 2,		/* legend and status lines */
	FPS		= 30,
	IdleMs		= 100,		/* redraw period when nothing is happening */
	Pollms		= 300,
	NToolNames	= 3,
	ActionHold	= 2500000000LL,	/* tint the status line this long */
	HistLife	= 10000000000LL,	/* keep actions this long */
	HistMax		= 32,
	WaveMax		= 16,
	WaveBand	= 2.6,		/* cells per colour band */
	WaveSpeed	= 1.4,		/* bands per second */
};

enum {
	Idle,
	Prefill,
	Decode,
	Offline,
};

/* harness/agent action classes, in legend order */
enum {
	ACmd,
	ARead,
	AWrite,
	ANet,
	AAgent,
	AMem,
	AView,
	AOther,
	NAction,
};

enum {
	StateIdx = 0,
	ErrIdx = NAction + 1,
	NPalette = NAction + 2,
};

static char *actionname[NAction] = {
	"cmd", "read", "write", "net", "agent", "mem", "view", "tool",
};

typedef struct Model Model;
struct Model {
	int	state;
	double	rate;
	char	status[80];
};

typedef struct Update Update;
struct Update {
	int	type;
	Model	model;		/* UModel */
	int	kind;		/* UAction */
	int	err;		/* UAction */
	int	done;		/* UAction: completion of an earlier action */
	vlong	when;		/* UAction */
	char	name[16];	/* UAction */
};

enum {
	UModel,
	UAction,
};

typedef struct Hist Hist;
struct Hist {
	int	kind;
	int	err;
	vlong	when;
	char	name[16];
};

static Image *back;
static Image *green;
static Image *amber;
static Image *dim;
static Image *red;
static Image *palette[NPalette];
static Font *f;
static int cw, ch;
static int gw, gh;

static char *baseurl;
static char *feedpath;
static Channel *upchan;
static int state = Idle;
static char status[80] = "idle";
static double rate;
static int verbose;

static int mlastdecoded;
static vlong mlastdecodedtime;

static vlong feedoff;
static int feedinit;
static char feedpartial[96];
static int feedpartialn;
static char toolnames[NToolNames][16];
static int ntoolnames;

static Hist hist[HistMax];
static int nhist;
static uchar waveidx[WaveMax];
static int nwave;
static double wavepos;
static int lastaction = -1;
static int lasterr;
static vlong lastactiontime;
static vlong lastframe;
static vlong lastdraw;

static int	actionkind(char*);
static int	actionactive(void);
static Image*	actioncolor(void);

/* libthread's default 8K main stack is too small for libdraw setup */
int mainstacksize = 64*1024;

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
pollmodel(Model *m)
{
	char buf[8192];
	JSON *j, *slot, *nt, *v;
	JSONEl *el;
	int proc, dec, ptok;

	m->rate = 0;
	if(fetch(baseurl, buf, sizeof buf) < 0){
		m->state = Offline;
		snprint(m->status, sizeof m->status, "no server");
		return;
	}

	j = jsonparse(buf);
	if(j == nil || j->t != JSONArray){
		if(j != nil)
			jsonfree(j);
		m->state = Offline;
		snprint(m->status, sizeof m->status, "bad slots");
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
		m->state = Idle;
		mlastdecoded = 0;
		mlastdecodedtime = 0;
		snprint(m->status, sizeof m->status, "idle");
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

			m->state = Decode;
			now = nsec();
			if(mlastdecodedtime != 0 && dec > mlastdecoded && now > mlastdecodedtime){
				dt = now - mlastdecodedtime;
				m->rate = (double)(dec - mlastdecoded) * 1000000000.0 / (double)dt;
			}
			mlastdecoded = dec;
			mlastdecodedtime = now;
			snprint(m->status, sizeof m->status, "decode %.1f tok/s", m->rate);
		} else {
			m->state = Prefill;
			mlastdecoded = 0;
			mlastdecodedtime = 0;
			ptok = 0;
			v = jsonbyname(slot, "n_prompt_tokens");
			if(v != nil && v->t == JSONNumber)
				ptok = v->n;
			snprint(m->status, sizeof m->status, "prefill %d tok", ptok);
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

static int
hasprefix(char *name, char *word)
{
	int n;

	n = strlen(word);
	return strncmp(name, word, n) == 0;
}

static int
actionkind(char *name)
{
	if(hasprefix(name, "cmd") || hasprefix(name, "plumb")
	|| hasprefix(name, "man") || hasprefix(name, "window")
	|| hasprefix(name, "rc") || hasprefix(name, "exec")
	|| hasprefix(name, "shell") || hasprefix(name, "run")
	|| hasprefix(name, "command"))
		return ACmd;
	if(hasprefix(name, "write") || hasprefix(name, "edit")
	|| hasprefix(name, "patch") || hasprefix(name, "create")
	|| hasprefix(name, "apply"))
		return AWrite;
	if(hasprefix(name, "fetch") || hasprefix(name, "searx")
	|| hasprefix(name, "web") || hasprefix(name, "curl")
	|| hasprefix(name, "hget"))
		return ANet;
	if(hasprefix(name, "agent") || hasprefix(name, "task")
	|| hasprefix(name, "delegate") || hasprefix(name, "summar"))
		return AAgent;
	if(hasprefix(name, "memory") || hasprefix(name, "skill")
	|| hasprefix(name, "todo") || hasprefix(name, "plan"))
		return AMem;
	if(hasprefix(name, "image") || hasprefix(name, "screen")
	|| hasprefix(name, "view") || hasprefix(name, "screenshot"))
		return AView;
	if(hasprefix(name, "read") || hasprefix(name, "list")
	|| hasprefix(name, "glob") || hasprefix(name, "grep")
	|| hasprefix(name, "find") || hasprefix(name, "open")
	|| hasprefix(name, "search"))
		return ARead;
	return AOther;
}

static int
knownkind(char *s)
{
	int i;

	for(i = 0; i < NAction; i++)
		if(strcmp(s, actionname[i]) == 0)
			return i;
	return -1;
}

static void
setaction(Update *u, int kind, char *name, char *state)
{
	int n;

	u->kind = kind;
	u->err = state != nil && (strcmp(state, "error") == 0
		|| strcmp(state, "fail") == 0);
	u->done = state != nil && (u->err
		|| strcmp(state, "end") == 0
		|| strcmp(state, "done") == 0
		|| strcmp(state, "ok") == 0
		|| strcmp(state, "manual") == 0);
	n = strlen(name);
	if(n > (int)sizeof u->name - 1)
		n = sizeof u->name - 1;
	memmove(u->name, name, n);
	u->name[n] = 0;
}

static int
parsefeedline(char *line, Update *u)
{
	char *f[4];
	char *name, *state;
	int n, nf, k;

	memset(u, 0, sizeof *u);
	u->type = UAction;
	u->when = nsec();

	/* tolerate CRLF and comment lines from a remote producer */
	n = strlen(line);
	while(n > 0 && (line[n-1] == '\r' || line[n-1] == '\n'))
		line[--n] = 0;
	while(*line == ' ' || *line == '\t')
		line++;
	if(*line == 0 || *line == '#')
		return 0;

	nf = tokenize(line, f, nelem(f));
	if(nf <= 0)
		return 0;

	if(nf >= 3 && (k = knownkind(f[0])) >= 0){
		name = f[1];
		state = f[2];
	} else {
		k = actionkind(f[0]);
		name = f[0];
		state = nf >= 2 ? f[1] : "start";
	}
	setaction(u, k, name, state);
	return 1;
}

static void
buildwave(void)
{
	int i;

	memset(waveidx, 0, sizeof waveidx);
	if(nhist == 0){
		nwave = 0;
		return;
	}
	/* keep the palette length fixed so pruning does not reshuffle it */
	nwave = WaveMax;
	for(i = 0; i < nhist && i < WaveMax; i++)
		waveidx[i] = hist[i].err ? ErrIdx : 1 + hist[i].kind;
}

static void
pushaction(Update *u)
{
	int i;

	if(nhist < HistMax)
		nhist++;
	for(i = nhist - 1; i > 0; i--)
		hist[i] = hist[i - 1];
	hist[0].kind = u->kind;
	hist[0].err = u->err;
	hist[0].when = u->when;
	strncpy(hist[0].name, u->name, sizeof hist[0].name - 1);
	hist[0].name[sizeof hist[0].name - 1] = 0;

	lastaction = u->kind;
	lasterr = u->err;
	lastactiontime = u->when;
	buildwave();
	remembertool(u->name);
}

/*
 * A completion or error belongs to the action already on the wave, not
 * to a new band.  Find the newest matching name and recolour it.
 */
static int
markaction(Update *u)
{
	int i;

	for(i = 0; i < nhist; i++){
		if(hist[i].kind == u->kind && strcmp(hist[i].name, u->name) == 0){
			hist[i].err = u->err;
			hist[i].when = u->when;
			lastaction = hist[i].kind;
			lasterr = u->err;
			lastactiontime = u->when;
			buildwave();
			return 1;
		}
	}
	return 0;
}

static void
prunehist(void)
{
	vlong now;
	int i, j;

	now = nsec();
	j = 0;
	for(i = 0; i < nhist; i++)
		if(now - hist[i].when <= HistLife)
			hist[j++] = hist[i];
	if(j != nhist){
		nhist = j;
		buildwave();
	}
}

static void
pollfeed(Channel *c)
{
	int fd, m, i;
	vlong n;
	char buf[512];
	Update u;

	if(feedpath == nil)
		return;
	fd = open(feedpath, OREAD);
	if(fd < 0)
		return;

	if(!feedinit){
		/* tail: ignore actions that predate us */
		feedoff = seek(fd, 0, 2);
		if(feedoff < 0)
			feedoff = 0;
		feedinit = 1;
		close(fd);
		return;
	}

	n = seek(fd, 0, 2);
	if(n < feedoff){
		feedoff = 0;
		feedpartialn = 0;
	}
	if(n > feedoff && seek(fd, feedoff, 0) >= 0){
		while((m = read(fd, buf, sizeof buf)) > 0){
			feedoff += m;
			for(i = 0; i < m; i++){
				if(buf[i] == '\n'){
					feedpartial[feedpartialn] = 0;
					if(parsefeedline(feedpartial, &u) && c != nil)
						send(c, &u);
					feedpartialn = 0;
				} else if(feedpartialn < (int)sizeof feedpartial - 1){
					feedpartial[feedpartialn++] = buf[i];
				}
			}
		}
	}
	close(fd);
}

static void
monitor(void *)
{
	Update u;

	for(;;){
		pollmodel(&u.model);
		u.type = UModel;
		send(upchan, &u);
		pollfeed(upchan);
		sleep(Pollms);
	}
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

static int
actionactive(void)
{
	return nhist > 0 && lastaction >= 0 && lastactiontime != 0
	    && nsec() - lastactiontime < ActionHold;
}

static Image*
actioncolor(void)
{
	if(actionactive())
		return palette[lasterr ? ErrIdx : 1 + lastaction];
	return statecolor();
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

static void
geom(void)
{
	int cols, rows;

	cols = (Dx(screen->r) - 8) / cw;
	rows = (Dy(screen->r) - 8) / ch - SH;
	if(cols < 20)
		cols = 20;
	if(rows < 6)
		rows = 6;
	if(cols > MaxGW)
		cols = MaxGW;
	if(rows > MaxGH)
		rows = MaxGH;
	gw = cols;
	gh = rows;
	if(back != nil){
		freeimage(back);
		back = nil;
	}
	back = allocimage(display, Rect(0,0,gw*cw,(gh+SH)*ch), screen->chan, 0, DNofill);
	if(back == nil)
		sysfatal("allocimage: %r");
}

static Point
origin(void)
{
	Point p;

	p = screen->r.min;
	if(Dx(screen->r) > gw*cw)
		p.x += (Dx(screen->r) - gw*cw) / 2;
	if(Dy(screen->r) > (gh+SH)*ch)
		p.y += (Dy(screen->r) - (gh+SH)*ch) / 2;
	return p;
}

/*
 * Paint the shape with colour bands built from the action history.
 * Distance from the centre selects a slot; advancing wavepos moves the
 * bands outward, so recent actions ripple across the shape.
 */
static void
overlay(char out[MaxGH][MaxGW+1], uchar style[MaxGH][MaxGW], int w, int h)
{
	double cx, cy, dx, dy, d;
	int x, y, slot;

	if(nwave == 0)
		return;
	cx = (w - 1) / 2.0;
	cy = (h - 1) / 2.0;
	for(y = 0; y < h; y++){
		for(x = 0; x < w; x++){
			if(out[y][x] == ' ')
				continue;
			dx = x - cx;
			dy = (y - cy) * ((double)ch / cw);
			d = sqrt(dx * dx + dy * dy);
			slot = (int)(d / WaveBand - wavepos) % nwave;
			if(slot < 0)
				slot += nwave;
			style[y][x] = waveidx[slot];
		}
	}
}

static void
legend(void)
{
	Point lp;
	int i;

	if(gw < 50)
		return;
	lp = Pt(0, gh*ch);
	for(i = 0; i < NAction; i++){
		lp = string(back, lp, palette[1 + i], ZP, f, actionname[i]);
		if(actionactive() && !lasterr && i == lastaction)
			lp = string(back, lp, palette[1 + i], ZP, f, "*");
		lp = string(back, lp, dim, ZP, f, " ");
	}
	lp = string(back, lp, palette[ErrIdx], ZP, f, "err");
	if(actionactive() && lasterr)
		lp = string(back, lp, palette[ErrIdx], ZP, f, "*");
}

static void
redraw(void)
{
	static char out[MaxGH][MaxGW+1];
	static uchar style[MaxGH][MaxGW];
	char run[MaxGW+1];
	char line[128];
	Point p;
	Image *fg, *base, *im;
	int x, y, i, n;

	scenerender(out, style, gw, gh);
	overlay(out, style, gw, gh);

	fg = actioncolor();
	base = statecolor();
	draw(back, back->r, display->black, nil, ZP);
	for(y = 0; y < gh; y++){
		for(x = 0; x < gw; ){
			if(out[y][x] == ' '){
				x++;
				continue;
			}
			i = x;
			while(i < gw && out[y][i] != ' ' && style[y][i] == style[y][x])
				i++;
			n = i - x;
			memmove(run, &out[y][x], n);
			run[n] = 0;
			im = style[y][x] == StateIdx ? base : palette[style[y][x]];
			string(back, Pt(x*cw, y*ch), im, ZP, f, run);
			x = i;
		}
	}

	legend();

	n = snprint(line, sizeof line, "%s", status);
	if(ntoolnames > 0 && n < (int)sizeof line)
		n += snprint(line + n, sizeof line - n, " |");
	for(i = 0; i < ntoolnames && n < (int)sizeof line; i++)
		n += snprint(line + n, sizeof line - n, " %s", toolnames[i]);
	string(back, Pt(0, (gh+1)*ch), fg, ZP, f, line);

	p = origin();
	draw(screen, Rect(p.x, p.y, p.x + gw*cw, p.y + (gh+SH)*ch), back, nil, ZP);
	flushimage(display, 1);
}

static void
applyupdate(Update *u)
{
	if(u->type == UModel){
		state = u->model.state;
		rate = u->model.rate;
		snprint(status, sizeof status, "%s", u->model.status);
		setspeed();
		if(verbose)
			fprint(2, "model %s\n", status);
		return;
	}
	if(verbose)
		fprint(2, "action %s %s%s%s\n", actionname[u->kind], u->name,
			u->done ? " done" : "", u->err ? " error" : "");
	if(u->done && markaction(u))
		return;
	pushaction(u);
}

void
eresized(int new)
{
	if(new && getwindow(display, Refnone) < 0)
		sysfatal("resize: %r");
	if(cw > 0 && ch > 0){
		geom();
		redraw();
	}
}

static void
usage(void)
{
	fprint(2, "usage: %s [-t|-s] [-a] [-v] [-c action] [-p line] [-g shape] "
		"[-d seed] [-u baseurl] [-f toolfeed]\n", argv0);
	exits("usage");
}

static void
printframe(void)
{
	static char out[MaxGH][MaxGW+1];
	static uchar style[MaxGH][MaxGW];
	int y;

	scenerender(out, style, GW, GH);
	for(y = 0; y < GH; y++)
		print("%s\n", out[y]);
}

static void
demoactions(void)
{
	static char *names[] = {
		"cmd", "read", "write", "web_search", "agent", "memory",
		"screenshot", "frobnicate",
	};
	Update u;
	int i;

	for(i = 0; i < nelem(names); i++){
		memset(&u, 0, sizeof u);
		u.type = UAction;
		u.when = nsec();
		setaction(&u, actionkind(names[i]), names[i], "start");
		pushaction(&u);
	}
}

void
threadmain(int argc, char **argv)
{
	Event e;
	Update u;
	ulong tk, seed;
	char *classify, *pline, *shape;
	vlong now, dt;
	int test, all, statusmode, i;

	test = 0;
	all = 0;
	statusmode = 0;
	classify = nil;
	pline = nil;
	shape = nil;
	seed = 0;
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
	case 'v':
		verbose = 1;
		break;
	case 'c':
		classify = EARGF(usage());
		break;
	case 'p':
		pline = EARGF(usage());
		break;
	case 'g':
		shape = EARGF(usage());
		break;
	case 'd':
		seed = strtoul(EARGF(usage()), nil, 0);
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

	if(classify != nil){
		print("%s\n", actionname[actionkind(classify)]);
		exits(nil);
	}

	if(pline != nil){
		if(parsefeedline(pline, &u))
			print("%s %s %s\n", actionname[u.kind], u.name,
				u.err ? "error" : (u.done ? "done" : "start"));
		else
			print("empty\n");
		exits(nil);
	}

	if(shape != nil && scenesetshape(shape) < 0){
		fprint(2, "%s: unknown shape %s\n", argv0, shape);
		usage();
	}

	if(baseurl == nil)
		baseurl = getenv("oaiurl");
	if(baseurl == nil)
		baseurl = "http://127.0.0.1:8080";
	if(feedpath == nil)
		feedpath = getenv("oaitoolfeed");
	if(feedpath == nil)
		feedpath = "/tmp/oai.toolcalls";

	scenesrand(seed != 0 ? seed : nsec());
	scenereset();

	if(test){
		printframe();
		exits(nil);
	}

	if(statusmode){
		Model m;

		for(i = 0; i < 20; i++){
			pollmodel(&m);
			print("%s\n", m.status);
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
	scenesetaspect((double)ch / cw);

	green = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x33FF88FF);
	amber = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0xFFB000FF);
	dim = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x226644FF);
	red = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0xFF5555FF);
	if(green == nil || amber == nil || dim == nil || red == nil)
		sysfatal("allocimage: %r");

	palette[StateIdx] = nil;
	palette[1 + ACmd] = amber;
	palette[1 + ARead] = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x66CCFFFF);
	palette[1 + AWrite] = green;
	palette[1 + ANet] = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0xB388FFFF);
	palette[1 + AAgent] = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0xFF66CCFF);
	palette[1 + AMem] = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0xFFE066FF);
	palette[1 + AView] = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x88CCFFFF);
	palette[1 + AOther] = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0xCCCCCCFF);
	palette[ErrIdx] = red;
	for(i = 1; i < NPalette; i++)
		if(palette[i] == nil)
			sysfatal("allocimage: %r");

	back = nil;
	geom();

	if(all)
		demoactions();

	upchan = chancreate(sizeof(Update), 64);
	if(upchan == nil)
		sysfatal("chancreate: %r");
	if(proccreate(monitor, nil, 65536) < 0)
		sysfatal("proccreate: %r");

	einit(Emouse|Ekeyboard);
	tk = etimer(0, 1000/FPS);

	lastframe = nsec();
	lastdraw = lastframe;
	redraw();

	for(;;){
		switch(eread(Emouse|Ekeyboard|tk, &e)){
		case Ekeyboard:
			if(e.kbdc == 'q' || e.kbdc == 0x7f)
				threadexitsall(nil);
			break;
		case Emouse:
			break;
		default:
			for(;;){
				if(nbrecv(upchan, &u) <= 0)
					break;
				applyupdate(&u);
			}
			now = nsec();
			dt = now - lastframe;
			lastframe = now;
			sceneadvance((double)dt / 1000000000.0);
			wavepos += WaveSpeed * (double)dt / 1000000000.0;
			prunehist();
			if(state == Decode || state == Prefill || nhist > 0
			|| now - lastdraw >= IdleMs * 1000000LL){
				redraw();
				lastdraw = now;
			}
			break;
		}
	}
}
