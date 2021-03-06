#include "ODECustomGeometry.h"
#include "ODECommon.h"
#include <ode/collision.h>
#include <Timer.h>
#include <errors.h>
using namespace std;

//if a normal has this length then it is ignored
const static Real gZeroNormalTolerance = 1e-4;

//if two contact points are closer than this threshold, will try to look
//at the local geometry to derive a contact normal
const static Real gNormalFromGeometryTolerance = 1e-5;
//const static Real gNormalFromGeometryTolerance = 1e-2;

//if a barycentric coordinate is within this tolerance of zero, it will be
//considered a zero
const static Real gBarycentricCoordZeroTolerance = 1e-3;

//if true, takes the ODE tolerance points and performs additional contact
//checking -- useful for flat contacts
const static bool gDoTriangleTriangleCollisionDetection = false;

//doesn't consider unique contact points if they are between this tolerance
const static Real cptol=1e-5;

int gdCustomGeometryClass = 0;

void ReverseContact(dContactGeom& contact)
{
  std::swap(contact.g1,contact.g2);
  for(int k=0;k<3;k++) contact.normal[k]*=-1.0;
  std::swap(contact.side1,contact.side2);
}

dGeomID dCreateCustomGeometry(AnyCollisionGeometry3D* geometry,Real outerMargin)
{
  dGeomID geom = dCreateGeom(gdCustomGeometryClass);
  CustomGeometryData* data = dGetCustomGeometryData(geom);

  if(geometry->type == AnyGeometry3D::TriangleMesh) {
    AnyCast<CollisionMesh>(&geometry->collisionData)->CalcIncidentTris();
    AnyCast<CollisionMesh>(&geometry->collisionData)->CalcTriNeighbors();
  }

  data->geometry = geometry;
  data->outerMargin = outerMargin;
  dGeomSetCategoryBits(geom,0xffffffff);
  dGeomSetCollideBits(geom,0xffffffff);
  dGeomEnable(geom);
  return geom;
}

CustomGeometryData* dGetCustomGeometryData(dGeomID o)
{
  return (CustomGeometryData*)dGeomGetClassData(o);
}



//1 = pt, 2 = edge, 3 = face
inline int FeatureType(const Vector3& b) 
{
  int type=0;
  if(FuzzyZero(b.x,gBarycentricCoordZeroTolerance)) type++;
  if(FuzzyZero(b.y,gBarycentricCoordZeroTolerance)) type++;
  if(FuzzyZero(b.z,gBarycentricCoordZeroTolerance)) type++;
  return 3-type;
}

int EdgeIndex(const Vector3& b)
{
  if(FuzzyZero(b.x,gBarycentricCoordZeroTolerance)) return 0;
  if(FuzzyZero(b.y,gBarycentricCoordZeroTolerance)) return 1;
  if(FuzzyZero(b.z,gBarycentricCoordZeroTolerance)) return 2;
  return 0;
  FatalError("Shouldn't get here");
  return -1;
}

int VertexIndex(const Vector3& b)
{
  if(FuzzyEquals(b.x,One,gBarycentricCoordZeroTolerance)) return 0;
  if(FuzzyEquals(b.y,One,gBarycentricCoordZeroTolerance)) return 1;
  if(FuzzyEquals(b.z,One,gBarycentricCoordZeroTolerance)) return 2;
  return 0;
  FatalError("Shouldn't get here");
  return -1;
}

Vector3 VertexNormal(const CollisionMesh& m,int tri,int vnum)
{
  Assert(!m.incidentTris.empty());
  int v=m.tris[tri][vnum];
  Vector3 n(Zero);
  for(size_t i=0;i<m.incidentTris[v].size();i++)
    n += m.TriangleNormal(m.incidentTris[v][i]);
  n.inplaceNormalize();
  return m.currentTransform.R*n;
}

Vector3 EdgeNormal(const CollisionMesh& m,int tri,int e)
{
  Assert(!m.triNeighbors.empty());
  Vector3 n=m.TriangleNormal(tri);
  if(m.triNeighbors[tri][e] != -1) {
    n += m.TriangleNormal(m.triNeighbors[tri][e]);
    n.inplaceNormalize();
  }
  return m.currentTransform.R*n;
}

///Compute normal from mesh geometry: returns the local normal needed for
///triangle 1 on m1 to get out of triangle 2 on m2.
///p1 and p2 are given in local coordinates
Vector3 ContactNormal(const CollisionMesh& m1,const CollisionMesh& m2,const Vector3& p1,const Vector3& p2,int t1,int t2)
{
  Triangle3D tri1,tri2;
  m1.GetTriangle(t1,tri1);
  m2.GetTriangle(t2,tri2);
  Vector3 b1=tri1.barycentricCoords(p1);
  Vector3 b2=tri2.barycentricCoords(p2);
  int type1=FeatureType(b1),type2=FeatureType(b2);
  switch(type1) {
  case 1:  //pt
    switch(type2) {
    case 1:  //pt
      //get the triangle normals
      {
	//printf("ODECustomMesh: Point-point contact\n");
	Vector3 n1 = VertexNormal(m1,t1,VertexIndex(b1));
	Vector3 n2 = VertexNormal(m2,t2,VertexIndex(b2));
	n2 -= n1;
	n2.inplaceNormalize();
	return n2;
      }
      break;
    case 2:  //edge
      {
	//printf("ODECustomMesh: Point-edge contact\n");
	Vector3 n1 = VertexNormal(m1,t1,VertexIndex(b1));
	int e = EdgeIndex(b2);
	Segment3D s = tri2.edge(e);
	Vector3 ev = m2.currentTransform.R*(s.b-s.a);
	Vector3 n2 = EdgeNormal(m2,t2,e);
	n2-=(n1-ev*ev.dot(n1)/ev.dot(ev)); //project onto normal
	n2.inplaceNormalize();
	return n2;
      }
      break;
    case 3:  //face
      return m2.currentTransform.R*tri2.normal();
    }
    break;
  case 2:  //edge
    switch(type2) {
    case 1:  //pt
      {
	//printf("ODECustomMesh: Edge-point contact\n");
	Vector3 n2 = VertexNormal(m2,t2,VertexIndex(b2));
	int e = EdgeIndex(b1);
	Segment3D s = tri1.edge(e);
	Vector3 ev = m1.currentTransform.R*(s.b-s.a);
	Vector3 n1 = EdgeNormal(m1,t1,e);
	n2 = (n2-ev*ev.dot(n2)/ev.dot(ev))-n1; //project onto normal
	n2.inplaceNormalize();
	return n2;
      }
      break;
    case 2:  //edge
      {
	//printf("ODECustomMesh: Edge-edge contact\n");
	int e = EdgeIndex(b1);
	Segment3D s1 = tri1.edge(e);
	Vector3 ev1 = m1.currentTransform.R*(s1.b-s1.a);
	ev1.inplaceNormalize();
	e = EdgeIndex(b2);
	Segment3D s2 = tri2.edge(e);
	Vector3 ev2 = m2.currentTransform.R*(s2.b-s2.a);
	ev2.inplaceNormalize();
	Vector3 n; 
	n.setCross(ev1,ev2);
	Real len = n.length();
	if(len < gZeroNormalTolerance) {
	  //hmm... edges are parallel?
	}
	n /= len;
	//make sure the normal direction points into m1 and out of m2
	if(n.dot(m1.currentTransform*s1.a) < n.dot(m2.currentTransform*s2.a))
	  n.inplaceNegative();
	/*
	if(n.dot(m1.currentTransform.R*tri1.normal()) > 0.0) {
	  if(n.dot(m2.currentTransform.R*tri2.normal()) > 0.0) {
	    printf("ODECustomMesh: Warning, inconsistent normal direction? %g, %g\n",n.dot(m1.currentTransform.R*tri1.normal()),n.dot(m2.currentTransform.R*tri2.normal()));
	  }
	  n.inplaceNegative();
	}
	else {
	  if(n.dot(m2.currentTransform.R*tri2.normal()) < 0.0) {
	    printf("ODECustomMesh: Warning, inconsistent normal direction? %g, %g\n",n.dot(m1.currentTransform.R*tri1.normal()),n.dot(m2.currentTransform.R*tri2.normal()));
	  }
	}
	*/
	//cout<<"Edge vector 1 "<<ev1<<", vector 2" <<ev2<<", normal: "<<n<<endl;
	return n;
      }
      break;
    case 3:  //face
      return m2.currentTransform.R*tri2.normal();
    }
    break;
  case 3:  //face
    if(type2 == 3)
      printf("ODECustomMesh: Warning, face-face contact?\n");
    return m1.currentTransform.R*(-tri1.normal());
  }
  static int warnedCount = 0;
  if(warnedCount % 10000 == 0) 
    printf("ODECustomMesh: Warning, degenerate triangle, types %d %d\n",type1,type2);
  warnedCount++;
  //AssertNotReached();
  return Vector3(Zero);
}

//Returns a contact normal for the closest point to the triangle t.  p is the point on the triangle.
//The direction is the one in which triangle 1 can move to get away from closestpt
Vector3 ContactNormal(const CollisionMesh& m,const Vector3& p,int t,const Vector3& closestPt)
{
  Triangle3D tri;
  m.GetTriangle(t,tri);
  Vector3 b=tri.barycentricCoords(p);
  int type=FeatureType(b);
  switch(type) {
  case 1:  //pt
    //get the triangle normal
    {
      Vector3 n = VertexNormal(m,t,VertexIndex(b));
      n.inplaceNegative();
      return n;
    }
    break;
  case 2:  //edge
    {
      int e = EdgeIndex(b);
      Vector3 n = EdgeNormal(m,t,e);
      n.inplaceNegative();
      return n;
    }
    break;
  case 3:  //face
    return m.currentTransform.R*(-tri.normal());
  }
  static int warnedCount = 0;
  if(warnedCount % 10000 == 0) 
    printf("ODECustomMesh: Warning, degenerate triangle, types %d\n",type);
  warnedCount++;
  //AssertNotReached();
  return Vector3(Zero);
}

int MeshMeshCollide(CollisionMesh& m1,Real outerMargin1,CollisionMesh& m2,Real outerMargin2,dContactGeom* contact,int maxcontacts)
{
  CollisionMeshQuery q(m1,m2);
  bool res=q.WithinDistanceAll(outerMargin1+outerMargin2);
  if(!res) {
    return 0;
  }

  vector<int> t1,t2;
  vector<Vector3> cp1,cp2;
  q.TolerancePairs(t1,t2);
  q.TolerancePoints(cp1,cp2);
  //printf("%d Collision pairs\n",t1.size());
  const RigidTransform& T1 = m1.currentTransform;
  const RigidTransform& T2 = m2.currentTransform;
  RigidTransform T21; T21.mulInverseA(T1,T2);
  RigidTransform T12; T12.mulInverseA(T2,T1);
  Real tol = outerMargin1+outerMargin2;
  Real tol2 = Sqr(tol);

  size_t imax=t1.size();
  Triangle3D tri1,tri2,tri1loc,tri2loc;
  if(gDoTriangleTriangleCollisionDetection) {
    //test if more triangle vertices are closer than tolerance
    for(size_t i=0;i<imax;i++) {
      m1.GetTriangle(t1[i],tri1);
      m2.GetTriangle(t2[i],tri2);
      
      tri1loc.a = T12*tri1.a;
      tri1loc.b = T12*tri1.b;
      tri1loc.c = T12*tri1.c;
      tri2loc.a = T21*tri2.a;
      tri2loc.b = T21*tri2.b;
      tri2loc.c = T21*tri2.c;
      bool usecpa,usecpb,usecpc,usecpa2,usecpb2,usecpc2;
      Vector3 cpa = tri1.closestPoint(tri2loc.a);
      Vector3 cpb = tri1.closestPoint(tri2loc.b);
      Vector3 cpc = tri1.closestPoint(tri2loc.c);
      Vector3 cpa2 = tri2.closestPoint(tri1loc.a);
      Vector3 cpb2 = tri2.closestPoint(tri1loc.b);
      Vector3 cpc2 = tri2.closestPoint(tri1loc.c);
      usecpa = (cpa.distanceSquared(tri2loc.a) < tol2);
      usecpb = (cpb.distanceSquared(tri2loc.b) < tol2);
      usecpc = (cpc.distanceSquared(tri2loc.c) < tol2);
      usecpa2 = (cpa2.distanceSquared(tri1loc.a) < tol2);
      usecpb2 = (cpb2.distanceSquared(tri1loc.b) < tol2);
      usecpc2 = (cpc2.distanceSquared(tri1loc.c) < tol2);
      //if already existing, disable it
      if(usecpa && cpa.isEqual(cp1[i],cptol)) usecpa=false;
      if(usecpb && cpb.isEqual(cp1[i],cptol)) usecpb=false;
      if(usecpc && cpc.isEqual(cp1[i],cptol)) usecpc=false;
      if(usecpa2 && cpa2.isEqual(cp2[i],cptol)) usecpa2=false;
      if(usecpb2 && cpb2.isEqual(cp2[i],cptol)) usecpb2=false;
      if(usecpc2 && cpc2.isEqual(cp2[i],cptol)) usecpc2=false;
      
      if(usecpa) {
	if(usecpb && cpb.isEqual(cpa,cptol)) usecpb=false;
	if(usecpc && cpc.isEqual(cpa,cptol)) usecpc=false;
      }
      if(usecpb) {
	if(usecpc && cpc.isEqual(cpb,cptol)) usecpc=false;
      }
      if(usecpa2) {
	if(usecpb2 && cpb2.isEqual(cpa2,cptol)) usecpb2=false;
	if(usecpc2 && cpc2.isEqual(cpa2,cptol)) usecpc2=false;
      }
      if(usecpb) {
	if(usecpc2 && cpc.isEqual(cpb2,cptol)) usecpc2=false;
      }
      
      if(usecpa) {
	t1.push_back(t1[i]);
	t2.push_back(t2[i]);
	cp1.push_back(cpa);
	cp2.push_back(tri2.a);
      }
      if(usecpb) {
	t1.push_back(t1[i]);
	t2.push_back(t2[i]);
	cp1.push_back(cpb);
	cp2.push_back(tri2.b);
      }
      if(usecpc) {
	t1.push_back(t1[i]);
	t2.push_back(t2[i]);
	cp1.push_back(cpc);
	cp2.push_back(tri2.c);
      }
      if(usecpa2) {
	t1.push_back(t1[i]);
	t2.push_back(t2[i]);
	cp1.push_back(tri1.a);
	cp2.push_back(cpa2);
      }
      if(usecpb2) {
	t1.push_back(t1[i]);
	t2.push_back(t2[i]);
	cp1.push_back(tri1.b);
	cp2.push_back(cpb2);
      }
      if(usecpc2) {
	t1.push_back(t1[i]);
	t2.push_back(t2[i]);
	cp1.push_back(tri1.c);
	cp2.push_back(cpc2);
      }
    }
    /*
    if(t1.size() != imax)
      printf("ODECustomMesh: Triangle vert checking added %d points\n",t1.size()-imax);
    */
    //getchar();
  }

  imax = t1.size();
  static int warnedCount = 0;
  for(size_t i=0;i<imax;i++) {
    m1.GetTriangle(t1[i],tri1);
    m2.GetTriangle(t2[i],tri2);

    tri1loc.a = T12*tri1.a;
    tri1loc.b = T12*tri1.b;
    tri1loc.c = T12*tri1.c;
    if(tri1loc.intersects(tri2)) { 
      if(warnedCount % 1000 == 0) {
	printf("ODECustomMesh: Triangles penetrate margin %g: can't trust contact detector\n",tol);
      }
      warnedCount++;
      /*
      //the two triangles intersect! can't trust results of PQP
      t1[i] = t1.back();
      t2[i] = t2.back();
      cp1[i] = cp1.back();
      cp2[i] = cp2.back();
      i--;
      imax--;
      */
    }
  }
  if(t1.size() != imax) {
    printf("ODECustomMesh: %d candidate points were removed due to collision\n",t1.size()-imax);
    t1.resize(imax);
    t2.resize(imax);
    cp1.resize(imax);
    cp2.resize(imax);
  }
  
  int k=0;  //count the # of contact points added
  for(size_t i=0;i<cp1.size();i++) {
    Vector3 p1 = T1*cp1[i];
    Vector3 p2 = T2*cp2[i];
    Vector3 n=p1-p2;
    Real d = n.norm();
    if(d < gNormalFromGeometryTolerance) {  //compute normal from the geometry
      n = ContactNormal(m1,m2,cp1[i],cp2[i],t1[i],t2[i]);
    }
    else if(d > tol) {  //some penetration -- we can't trust the result of PQP
      continue;
    }
    else n /= d;
    //check for invalid normals
    Real len=n.length();
    if(len < gZeroNormalTolerance || !IsFinite(len)) continue;
    //cout<<"Local Points "<<cp1[i]<<", "<<cp2[i]<<endl;
    //cout<<"Points "<<p1<<", "<<p2<<endl;
    //Real utol = (tol)*0.5/d + 0.5;
    //CopyVector(contact[k].pos,p1+utol*(p2-p1));
    CopyVector(contact[k].pos,0.5*(p1+p2) + ((outerMargin2 - outerMargin1)*0.5)*n);
    CopyVector(contact[k].normal,n);
    contact[k].depth = tol - d;
    if(contact[k].depth < 0) contact[k].depth = 0;
    //cout<<"Normal "<<n<<", depth "<<contact[i].depth<<endl;
    //getchar();
    k++;
    if(k == maxcontacts) break;
  }
  return k;
}

int MeshPointCloudCollide(CollisionMesh& m1,Real outerMargin1,CollisionPointCloud& pc2,Real outerMargin2,dContactGeom* contact,int maxcontacts)
{
  Real tol = outerMargin1 + outerMargin2;
  int k=0;
  vector<int> tris;
  Triangle3D tri,triw;
  for(size_t i=0;i<pc2.points.size();i++) {
    Vector3 pw = pc2.currentTransform*pc2.points[i];
    NearbyTriangles(m1,pw,tol,tris,maxcontacts-k);
    for(size_t j=0;j<tris.size();j++) {   
      m1.GetTriangle(tris[j],tri);
      triw.a = m1.currentTransform*tri.a;
      triw.b = m1.currentTransform*tri.b;
      triw.c = m1.currentTransform*tri.c;
      Vector3 cp = triw.closestPoint(pw);
      Vector3 n = cp - pw;
      Real d = n.length();
      if(d < gNormalFromGeometryTolerance) {  //compute normal from the geometry
	Vector3 plocal;
	m1.currentTransform.mulInverse(cp,plocal);
	n = ContactNormal(m1,plocal,tris[j],pw);
      }
      else if(d > tol) {  //some penetration -- we can't trust the result of PQP
	continue;
      }
      else n /= d;
      //migrate the contact point to the center of the overlap region
      CopyVector(contact[k].pos,0.5*(cp+pw) + ((outerMargin2 - outerMargin1)*0.5)*n);
      CopyVector(contact[k].normal,n);
      contact[k].depth = tol - d;
      k++;
      if(k == maxcontacts) break;
    }
  }
  return k;
}

int PointCloudMeshCollide(CollisionPointCloud& pc1,Real outerMargin1,CollisionMesh& m2,Real outerMargin2,dContactGeom* contact,int maxcontacts)
{
  int num = MeshPointCloudCollide(m2,outerMargin2,pc1,outerMargin1,contact,maxcontacts);
  for(int i=0;i<num;i++) ReverseContact(contact[i]);
  return num;
}


int dCustomGeometryCollide (dGeomID o1, dGeomID o2, int flags,
			   dContactGeom *contact, int skip)
{
  int m = (flags&0xffff);
  if(m == 0) m=1;
  //printf("CustomGeometry collide\n");
  CustomGeometryData* d1 = dGetCustomGeometryData(o1);
  CustomGeometryData* d2 = dGetCustomGeometryData(o2);
  RigidTransform T1;
  RigidTransform T2;
  CopyMatrix(T1.R,dGeomGetRotation(o1));
  CopyVector(T1.t,dGeomGetPosition(o1));
  CopyMatrix(T2.R,dGeomGetRotation(o2));
  CopyVector(T2.t,dGeomGetPosition(o2));
  d1->geometry->SetTransform(T1);
  d2->geometry->SetTransform(T2);

  int n=0;
  switch(d1->geometry->type) {
  case AnyGeometry3D::Primitive:
    switch(d2->geometry->type) {
    case AnyGeometry3D::Primitive:
      fprintf(stderr,"TODO: primitive-primitive collisions\n");
      break;
    case AnyGeometry3D::TriangleMesh:
      fprintf(stderr,"TODO: primitive-triangle mesh collisions\n");
      break;
    case AnyGeometry3D::PointCloud:
      fprintf(stderr,"TODO: primitive-point cloud collisions\n");
      break;
    case AnyGeometry3D::ImplicitSurface:
      fprintf(stderr,"TODO: primitive-implicit surface collisions\n");
      break;
    }
    break;
  case AnyGeometry3D::TriangleMesh:
    switch(d2->geometry->type) {
    case AnyGeometry3D::Primitive:
      fprintf(stderr,"TODO: triangle mesh-primitive collisions\n");
      break;
    case AnyGeometry3D::TriangleMesh:
      n = MeshMeshCollide(d1->geometry->TriangleMeshCollisionData(),d1->geometry->margin+d1->outerMargin,
			  d2->geometry->TriangleMeshCollisionData(),d2->geometry->margin+d2->outerMargin,
			  contact,m);
      break;
    case AnyGeometry3D::PointCloud:
      n = MeshPointCloudCollide(d1->geometry->TriangleMeshCollisionData(),d1->geometry->margin+d1->outerMargin,
				d2->geometry->PointCloudCollisionData(),d2->geometry->margin+d2->outerMargin,
				contact,m);
      break;
    case AnyGeometry3D::ImplicitSurface:
      fprintf(stderr,"TODO: triangle mesh-implicit surface collisions\n");
      break;
    }
    break;
  case AnyGeometry3D::PointCloud:
    switch(d2->geometry->type) {
    case AnyGeometry3D::Primitive:
      fprintf(stderr,"TODO: point cloud-primitive collisions\n");
      break;
    case AnyGeometry3D::TriangleMesh:
      n = PointCloudMeshCollide(*AnyCast<CollisionPointCloud>(&d1->geometry->collisionData),d1->geometry->margin+d1->outerMargin,
				   *AnyCast<CollisionMesh>(&d2->geometry->collisionData),d2->geometry->margin+d2->outerMargin,
				   contact,m);
      break;
    case AnyGeometry3D::PointCloud:
      fprintf(stderr,"TODO: point cloud-point cloud collisions\n");
      break;
    case AnyGeometry3D::ImplicitSurface:
      fprintf(stderr,"TODO: point cloud-implicit surface collisions\n");
      break;
    }
    break;
  case AnyGeometry3D::ImplicitSurface:
    switch(d2->geometry->type) {
    case AnyGeometry3D::Primitive:
      fprintf(stderr,"TODO: implicit surface-primitive collisions\n");
      break;
    case AnyGeometry3D::TriangleMesh:
      fprintf(stderr,"TODO: implicit surface-triangle mesh collisions\n");
      break;
    case AnyGeometry3D::PointCloud:
      fprintf(stderr,"TODO: implicit surface-point cloud collisions\n");
      break;
    case AnyGeometry3D::ImplicitSurface:
      fprintf(stderr,"TODO: implicit surface-implicit surface collisions\n");
      break;
    }
    break;
  }
  for(int k=0;k<n;k++) {
    contact[k].g1 = o1;
    contact[k].g2 = o2;
  }
  return n;
}


dColliderFn * dCustomGeometryGetColliderFn (int num)
{
  if(num == gdCustomGeometryClass) return dCustomGeometryCollide;
  else return NULL;
}


void dCustomGeometryAABB(dGeomID o,dReal aabb[6])
{
  CustomGeometryData* d = dGetCustomGeometryData(o);
  AABB3D bb;
  RigidTransform T;
  CopyMatrix(T.R,dGeomGetRotation(o));
  CopyVector(T.t,dGeomGetPosition(o));  
  d->geometry->SetTransform(T);
  bb = d->geometry->GetAABB();
  bb.bmin -= Vector3(d->outerMargin,d->outerMargin,d->outerMargin);
  bb.bmax += Vector3(d->outerMargin,d->outerMargin,d->outerMargin);
  aabb[0] = bb.bmin.x;
  aabb[1] = bb.bmax.x;
  aabb[2] = bb.bmin.y;
  aabb[3] = bb.bmax.y;
  aabb[4] = bb.bmin.z;
  aabb[5] = bb.bmax.z;
}



void dCustomGeometryDtor(dGeomID o)
{
}

void InitODECustomGeometry()
{
  dGeomClass mmclass;
  mmclass.bytes = sizeof(CustomGeometryData);
  mmclass.collider = dCustomGeometryGetColliderFn;
  mmclass.aabb = dCustomGeometryAABB;
  mmclass.aabb_test = NULL;
  mmclass.dtor = dCustomGeometryDtor;
  gdCustomGeometryClass = dCreateGeomClass(&mmclass);
}
