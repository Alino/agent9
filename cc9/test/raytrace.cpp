// raytrace — a multithreaded C++ path tracer for 9front (Peter Shirley's
// "Ray Tracing in One Weekend" scene). Pure C++: <cmath> + STL + std::thread
// (real rfork parallelism) + exceptions-capable runtime. No external libs, no
// Go equivalent — the kind of native C++ compute cc9 uniquely brings to Plan 9.
//   build: cc9 build cc9/test/raytrace.cpp ; run: raytrace /tmp/out.ppm
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <thread>
#include <atomic>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct V {
	double x=0,y=0,z=0;
	V(){} V(double a,double b,double c):x(a),y(b),z(c){}
	V operator+(V o)const{return{x+o.x,y+o.y,z+o.z};}
	V operator-(V o)const{return{x-o.x,y-o.y,z-o.z};}
	V operator*(double s)const{return{x*s,y*s,z*s};}
	V operator*(V o)const{return{x*o.x,y*o.y,z*o.z};}
	V operator-()const{return{-x,-y,-z};}
};
static double dot(V a,V b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static V cross(V a,V b){return{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
static double len(V a){return std::sqrt(dot(a,a));}
static V unit(V a){return a*(1.0/len(a));}

// per-thread xorshift RNG -> [0,1)
struct Rng{ uint64_t s; double d(){ s^=s<<13; s^=s>>7; s^=s<<17; return (s>>11)*(1.0/9007199254740992.0);} };
static V randUnit(Rng&r){ for(;;){ V p{r.d()*2-1,r.d()*2-1,r.d()*2-1}; if(dot(p,p)<1) return unit(p);} }

struct Ray{ V o,d; };
enum Mat{ LAMB, METAL, GLASS };
struct Sphere{ V c; double r; Mat m; V albedo; double fuzz; double ir; };

static std::vector<Sphere> world;

static bool hit(const Ray&ray,double tmin,double tmax,double&t,V&p,V&n,const Sphere*&hs){
	bool any=false;
	for(const auto&s:world){
		V oc=ray.o-s.c; double a=dot(ray.d,ray.d), hb=dot(oc,ray.d), c=dot(oc,oc)-s.r*s.r;
		double disc=hb*hb-a*c; if(disc<0)continue; double sq=std::sqrt(disc);
		double root=(-hb-sq)/a; if(root<tmin||root>tmax){ root=(-hb+sq)/a; if(root<tmin||root>tmax)continue; }
		t=root; tmax=root; p=ray.o+ray.d*t; n=(p-s.c)*(1.0/s.r); hs=&s; any=true;
	}
	return any;
}
static V reflect(V v,V n){return v-n*(2*dot(v,n));}
static V refract(V uv,V n,double eta){ double ct=std::fmin(dot(-uv,n),1.0); V perp=(uv+n*ct)*eta; V par=n*(-std::sqrt(std::fabs(1.0-dot(perp,perp)))); return perp+par; }
static double reflectance(double cos,double ir){ double r0=(1-ir)/(1+ir); r0*=r0; return r0+(1-r0)*std::pow(1-cos,5); }

static V rayColor(Ray ray,Rng&rng,int depth){
	V atten{1,1,1};
	for(int i=0;i<depth;i++){
		double t; V p,n; const Sphere*s;
		if(!hit(ray,1e-3,1e30,t,p,n,s)){
			V u=unit(ray.d); double a=0.5*(u.y+1.0);
			V sky=V{1,1,1}*(1-a)+V{0.5,0.7,1.0}*a;
			return atten*sky;
		}
		bool front=dot(ray.d,n)<0; V nn=front?n:-n;
		if(s->m==LAMB){ V dir=nn+randUnit(rng); if(std::fabs(dir.x)+std::fabs(dir.y)+std::fabs(dir.z)<1e-8)dir=nn; ray={p,unit(dir)}; atten=atten*s->albedo; }
		else if(s->m==METAL){ V refl=reflect(unit(ray.d),nn); ray={p,unit(refl+randUnit(rng)*s->fuzz)}; atten=atten*s->albedo; if(dot(ray.d,nn)<=0)return{0,0,0}; }
		else { double eta=front?(1.0/s->ir):s->ir; V u=unit(ray.d); double ct=std::fmin(dot(-u,nn),1.0),st=std::sqrt(1-ct*ct); V dir=(eta*st>1.0||reflectance(ct,eta)>rng.d())?reflect(u,nn):refract(u,nn,eta); ray={p,dir}; }
	}
	return{0,0,0};
}

int main(int argc,char**argv){
	const char*outp = argc>1?argv[1]:"/tmp/rt.ppm";
	int W=400,H=225,spp=24,depth=16;
	if(argc>2) spp=(int)strtol(argv[2],0,10);
	// scene: ground + three feature spheres + a few small ones
	world.push_back({{0,-1000,0},1000,LAMB,{0.5,0.5,0.5},0,0});
	world.push_back({{0,1,0},1.0,GLASS,{1,1,1},0,1.5});
	world.push_back({{-4,1,0},1.0,LAMB,{0.4,0.2,0.1},0,0});
	world.push_back({{4,1,0},1.0,METAL,{0.7,0.6,0.5},0.0,0});
	Rng sc{0x1234567};
	for(int a=-3;a<3;a++)for(int b=-3;b<3;b++){ double cm=sc.d(); V cen{a+0.9*sc.d(),0.2,b+0.9*sc.d()}; if(len(cen-V{4,0.2,0})<0.9)continue;
		if(cm<0.7) world.push_back({cen,0.2,LAMB,{sc.d()*sc.d(),sc.d()*sc.d(),sc.d()*sc.d()},0,0});
		else if(cm<0.9) world.push_back({cen,0.2,METAL,{0.5+0.5*sc.d(),0.5+0.5*sc.d(),0.5+0.5*sc.d()},0.3*sc.d(),0});
		else world.push_back({cen,0.2,GLASS,{1,1,1},0,1.5}); }

	V look{13,2,3}, at{0,0,0}, vup{0,1,0}; double vfov=20*M_PI/180, ar=double(W)/H;
	double vh=2*std::tan(vfov/2), vw=vh*ar;
	V w=unit(look-at), u=unit(cross(vup,w)), v=cross(w,u);
	V horiz=u*vw, vert=v*vh, llc=look-horiz*0.5-vert*0.5-w;

	std::vector<uint8_t> img(W*H*3);
	std::atomic<int> row{0};
	auto worker=[&](int seed){
		for(int j=row.fetch_add(1); j<H; j=row.fetch_add(1)){
			Rng rng{(uint64_t)(j*9781+seed*6271+1)};
			for(int i=0;i<W;i++){
				V col{0,0,0};
				for(int s=0;s<spp;s++){
					double su=(i+rng.d())/(W-1), sv=(j+rng.d())/(H-1);
					Ray r{look, unit(llc+horiz*su+vert*sv-look)};
					col=col+rayColor(r,rng,depth);
				}
				col=col*(1.0/spp);
				V g{std::sqrt(col.x),std::sqrt(col.y),std::sqrt(col.z)};   // gamma 2
				int idx=((H-1-j)*W+i)*3;
				img[idx+0]=(uint8_t)(255*(g.x<0?0:g.x>1?1:g.x));
				img[idx+1]=(uint8_t)(255*(g.y<0?0:g.y>1?1:g.y));
				img[idx+2]=(uint8_t)(255*(g.z<0?0:g.z>1?1:g.z));
			}
		}
	};
	unsigned nt=2; std::vector<std::thread> ts;
	for(unsigned k=0;k<nt;k++) ts.emplace_back(worker,k);
	for(auto&t:ts) t.join();

	FILE*f=fopen(outp,"w"); if(!f){ printf("cannot open %s\n",outp); return 1; }
	fprintf(f,"P6\n%d %d\n255\n",W,H); fwrite(img.data(),1,img.size(),f); fclose(f);
	printf("raytrace: %dx%d, %d spp, %d objects, %u threads -> %s\n",W,H,spp,(int)world.size(),nt,outp);
	return 0;
}
