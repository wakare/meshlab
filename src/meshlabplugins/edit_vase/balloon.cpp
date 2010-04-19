#include "balloon.h"
#include "float.h"
#include "math.h"
using namespace vcg;

//---------------------------------------------------------------------------------------//
//
//                                   LOGIC
//
//---------------------------------------------------------------------------------------//
void Balloon::init( int gridsize, int gridpad ){
    //--- Reset the iteration counter
    numiterscompleted = 0;

    //--- Instantiate a properly sized wrapping volume
    vol.init( gridsize, gridpad, cloud.bbox );
    qDebug() << "Created a volume of sizes: " << vol.size(0) << " " << vol.size(1) << " " << vol.size(2);

    //--- Compute hashing of ray intersections (using similar space structure of volume)
    gridAccell.init( vol, cloud );
    qDebug() << "Finished hashing rays into the volume";

    //--- Construct EDF of initial wrapping volume (BBOX)
    // Instead of constructing isosurface exactly on the bounding box, stay a bit large,
    // so that ray-isosurface intersections will not fail for that region.
    // Remember that rays will take a step JUST in their direction, so if they lie exactly
    // on the bbox, they would go outside. The code below corrects this from happening.
    Box3f enlargedbb = cloud.bbox;
    // float del = .99*vol.getDelta(); // almost +1 voxel in each direction
    float del = .50*vol.getDelta(); // ADEBUG: almost to debug correspondences
    Point3f offset( del,del,del );
    enlargedbb.Offset( offset );
    vol.initField( enlargedbb );    
    
    //--- Extract initial zero level set surface
    vol.isosurface( surf, 0 );
    qDebug() << "Extracted balloon isosurface (" << surf.vn << " " << surf.fn << ")";    

    //--- Re-compute isosurface so that marching-cube and EDF match
    // vol.initField(surf, gridAccell);
}
void Balloon::updateViewField(){
    //--- Setup the interpolation system
    float OMEGA = 1e8;
    finterp.Init( &surf, COTANGENT );
    // finterp.Init( &surf, COMBINATORIAL );

    //--- DEBUG: Create a dummy QUALITY field Q() on mesh vertices and add constraints
    #if false
        rm |= SURF_VCOLOR;
        float OMEGA = 100;
        tri::UpdateQuality<CMeshO>::VertexConstant(surf, 0);
        for( int i=0; i<surf.vert.size(); i++ ){
            CVertexO& v = surf.vert[i];
            float f = v.P()[1];
            if( fabs(f)>.9 ){
                v.Q() = f;
                // rfinterp.AddConstraint( i, OMEGA, f );
                //qDebug() << "active constraints at vertex " << i << " val: " << f << endl;
            }
        }
        // Map vertex quality to a color
        Histogramf H;
        tri::Stat<CMeshO>::ComputePerVertexQualityHistogram(surf,H);
        tri::UpdateColor<CMeshO>::VertexQualityRamp(surf,H.Percentile(0.0f),H.Percentile(1.0f));
        qDebug() << H.Percentile(1.0f);
        return;
    #endif

    // Shared data
    enum INITMODE {TESTINTERP, FACEINTERSECTIONS, BOXINTERSECTIONS} mode;
    mode = FACEINTERSECTIONS;
    const float ONETHIRD = 1.0f/3.0f;
    float t,u,v; // Ray-Triangle intersection parameters
    Point3f fcenter;
    Point3i off;


    // Uses the face centroid as a hash key in the accellGrid to determine which
    // rays might intersect the current face.
    if( mode == BOXINTERSECTIONS ){
        for(CMeshO::FaceIterator fi=surf.face.begin();fi!=surf.face.end();fi++){
            CFaceO& f = *(fi);
            fcenter = f.P(0) + f.P(1) + f.P(2);
            fcenter = myscale( fcenter, ONETHIRD );
            gridAccell.pos2off( fcenter, off );
            //--- examine intersections and determine real ones...
            PointerVector& rays = gridAccell.Val(off[0], off[1], off[2]);
            f.C() = ( rays.size()>0 ) ? Color4b(255,0,0,255) : Color4b(255,255,255,255);
        }
        qDebug() << "WARNING: test-mode only, per vertex field not updated!!";
    }
    else if( mode == FACEINTERSECTIONS ){
        this->rm ^= SURF_VCOLOR;
        this->rm |= SURF_FCOLOR;
        surf.face.EnableColor();
        surf.face.EnableQuality();
        tri::UpdateQuality<CMeshO>::FaceConstant(surf, 0);
        std::vector<float> tot_w( surf.fn, 0 );
        for(CMeshO::FaceIterator fi=surf.face.begin();fi!=surf.face.end();fi++){
            CFaceO& f = *(fi);
            f.ClearS();
            f.C() = Color4b(255,255,255, 255);
            f.Q() = 0; // initialize
            Point3f fcenter = f.P(0) + f.P(1) + f.P(2);
            fcenter = myscale( fcenter, ONETHIRD );
            gridAccell.pos2off( fcenter, off );
            PointerVector& rays = gridAccell.Val(off[0], off[1], off[2]);
            // Each intersecting ray gives a contribution on the face to each of the
            // face vertices according to the barycentric weights
            for(unsigned int i=0; i<rays.size(); i++)
                if( vcg::IntersectionRayTriangle<float>(*rays[i], f.P(0), f.P(1), f.P(2), t, u, v) ){
                    // Color the faces, if more than one, take average
                    tot_w[ tri::Index(surf,f) ]++;
                    f.Q() += t; // normalize with tot_w after
                    f.SetS();
                    //--- Add the constraints to the field
                    finterp.AddConstraint( tri::Index(surf,f.V(0)), OMEGA*(1-u-v), t );
                    finterp.AddConstraint( tri::Index(surf,f.V(1)), OMEGA*(u), t );
                    finterp.AddConstraint( tri::Index(surf,f.V(2)), OMEGA*(v), t );
                }
        }

        //--- Normalize in case there is more than 1 ray per-face
        for(CMeshO::FaceIterator fi=surf.face.begin();fi!=surf.face.end();fi++){
            if( tot_w[ tri::Index(surf,*fi) ] > 0 )
                fi->Q() /= tot_w[ tri::Index(surf,*fi) ];
        }
        //--- Transfer the average distance stored in face quality to a color
        tri::UpdateColor<CMeshO>::FaceQualityRamp(surf, true);
    }
}
void Balloon::interpolateField(){
    //--- Interpolate the field
    // qDebug() << "Solving field interpolation";
    finterp.Solve();

    if( false ){
//        //--- Perform exponential (max) distance mapping
//        float maxdst = -FLT_MAX, mindst = FLT_MAX;
//        for(CMeshO::VertexIterator vi=surf.vert.begin(); vi!=surf.vert.end(); vi++)
//            maxdst = ((*vi).Q()>maxdst) ? (*vi).Q() : maxdst;
//        float sigma2 = vol.getDelta(); sigma2*=sigma2;
//        for(CMeshO::VertexIterator vi=surf.vert.begin(); vi!=surf.vert.end(); vi++)
//            (*vi).Q() = exp( - powf((*vi).Q()-maxdst,2) / sigma2 );
//
//        maxdst = -FLT_MAX; mindst = FLT_MAX;
//        for(CMeshO::VertexIterator vi=surf.vert.begin(); vi!=surf.vert.end(); vi++){
//            maxdst = ((*vi).Q()>maxdst) ? (*vi).Q() : maxdst;
//            mindst = ((*vi).Q()<mindst) ? (*vi).Q() : mindst;
//        }
        // qDebug() << mindst << " " << maxdst;

        //--- Transfer vertex quality to surface
        rm &= ~SURF_FCOLOR; // disable face colors
        rm |= SURF_VCOLOR; // enable vertex colors
        tri::UpdateColor<CMeshO>::VertexQualityRamp(surf,0,1);
        // qDebug() << "Exp transform showed as vertex color!";
    }
    else{
        //--- Transfer vertex quality to surface
        rm &= ~SURF_FCOLOR; // disable face colors
        rm |= SURF_VCOLOR; // enable vertex colors
        Histogram<float> H;
        tri::Stat<CMeshO>::ComputePerVertexQualityHistogram(surf,H);
        tri::UpdateColor<CMeshO>::VertexQualityRamp(surf,H.Percentile(0.0f),H.Percentile(1.0f));
        // qDebug() << "Interpolated field showed as vertex color!";
    }
}
void Balloon::evolveBalloon(){

//    // Compute normalized Mean-Curvature
//    for(CMeshO::VertexIterator vi = surf.vert.begin(); vi != surf.vert.end(); ++vi){
//        VertexCurvature(&*vi,true);
//        (*vi).Q() = (*vi).Kh();
//    }
//    Histogram<float> H;
//    tri::Stat<CMeshO>::ComputePerVertexQualityHistogram(surf,H);
//    tri::UpdateColor<CMeshO>::VertexQualityRamp(surf,H.Percentile(0.0f),H.Percentile(1.0f));
//    return;

    // Test whether applying it more than one time does nothing (1-1 conversion)
    // vol.initField(surf, gridAccell); return;

    // Update iteration counter
    numiterscompleted++;

    //--- Compute active band around surface and correspondences
    // Memory estimation: we move at most by 1 voxel, so we need to update at least the 2 neighborhood
    // of a voxel on each side so that the implicit function will be correct, this resulting in 2 voxels
    // per side thus: 2+2+1 per face.
    std::vector<Point3i> band;
    band.reserve(5*surf.fn);
    vol.updateSurfaceCorrespondence( surf, gridAccell, 2*vol.getDelta(), band );

    //--- Compute updates from amount stored in vertex quality
    float a,b,c;
    Point3f voxp;
    std::vector<float> updates(band.size(),0);
    float maxdst = -FLT_MAX, mindst = FLT_MAX;
    for(unsigned int i=0; i<band.size(); i++){
        Point3i& voxi = band[i];
        MyVoxel& v = vol.Voxel(voxi);
        CFaceO& f = *v.face;
        // extract projected points on surface and obtain barycentric coordinates
        // TODO: double work, it was already computed during correspodence, write a new function?
        Point3f proj;
        vol.off2pos(voxi, voxp);
        vcg::SignedFacePointDistance(f, voxp, proj);
        Triangle3<float> triFace( f.P(0), f.P(1), f.P(2) );
        vcg::InterpolationParameters(triFace, proj, a,b,c);
        updates[i] = a*f.V(0)->Q() + b*f.V(1)->Q() + c*f.V(2)->Q();
        // qDebug("from face %d - %.2f", Index(surf, f), val);

        //--- Zero the band information so that next extraction will be successfull
        v.status = 0;
        v.face = 0;
        v.index = 0;

        //--- Keep track of max/min of updates[]
        maxdst = (updates[i]>maxdst) ? updates[i] : maxdst;
        mindst = (updates[i]<mindst) ? updates[i] : mindst;
    }

    //--- Apply exponential functions to modulate and regularize the updates
    float sigma2 = vol.getDelta(); sigma2*=sigma2;
    float k1, k2;
    for(unsigned int i=0; i<band.size(); i++){
        Point3i& voxi = band[i];
        MyVoxel& v = vol.Voxel(voxi);
        //--- Faster if further
        k1 = exp( -powf(updates[i]-maxdst,2) / (3*sigma2) );
        //--- Slowdown weight (according to distance from surface)
        k2 = 1 - exp( -powf(updates[i],2) / sigma2 );

        //--- Update
        v.field += .25*k1*k2;
    }

    //--- Extract initial zero level set surface
    vol.isosurface( surf, 0 );
}

//---------------------------------------------------------------------------------------//
//
//                                   RENDERING
//
//---------------------------------------------------------------------------------------//
void Balloon::render_cloud(){
    // Draw the ray/rays from their origin up to some distance away
    glDisable(GL_LIGHTING);
    glColor3f(.5, .5, .5);
    for(CMeshO::VertexIterator vi=cloud.vert.begin(); vi!=cloud.vert.end(); ++vi){
        Point3f p1 = (*vi).P();
        Point3f n = (*vi).N();
        // n[0] *= .1; n[1] *= .1; n[2] *= .1; // Scale the viewdir
        Point3f p2 = (*vi).P() + n;
        glBegin(GL_LINES);
            glVertex3f(p1[0],p1[1],p1[2]);
            glVertex3f(p2[0],p2[1],p2[2]);
        glEnd();
    }
    glEnable(GL_LIGHTING);
}
void Balloon::render_isosurface(GLArea* gla){
    GLW::DrawMode       dm = GLW::DMFlatWire;
    GLW::ColorMode      cm = GLW::CMPerVert;
    GLW::TextureMode	tm = GLW::TMNone;

    // By default vertColor is defined, so let's check if we need/want to
    // draw the face colors first.
    if( (rm & SURF_FCOLOR) && tri::HasPerFaceColor(surf) ){
        gla->rm.colorMode = vcg::GLW::CMPerFace; // Corrects MESHLAB BUG
        cm = GLW::CMPerFace;
    }
    else if( (rm & SURF_VCOLOR) && tri::HasPerVertexColor(surf) ){
        gla->rm.colorMode = vcg::GLW::CMPerVert; // Corrects MESHLAB BUG
        cm = GLW::CMPerVert;
    }
    GlTrimesh<CMeshO> surf_renderer;
    surf_renderer.m = &surf;
    surf_renderer.Draw(dm, cm, tm);
}
void Balloon::render_surf_to_acc(){
    gridAccell.render();

#if 0
    glDisable( GL_LIGHTING );
    const float ONETHIRD = 1.0f/3.0f;
    Point3f fcenter;
    Point3i off, o;
    glColor3f(1.0, 0.0, 0.0);
    for(unsigned int fi =0; fi<surf.face.size(); fi++){
        if( fi!= 5 )
            continue;
        CFaceO& f = surf.face[fi];
        fcenter = f.P(0) + f.P(1) + f.P(2);
        fcenter = myscale( fcenter, ONETHIRD );
        gridAccell.pos2off( fcenter, off );
        vol.pos2off( fcenter, o );
        gridAccell.off2pos( off, fcenter );
        vcg::drawBox( fcenter, vol.getDelta()*.95, true );


        // SHO BLOCK
        qDebug() << "full volume: " << endl << vol.toString(2, o[2]);
        // DEBUG: examine field values around current coordinate
        QString q;       
        qDebug();
        q.sprintf("%2.2f %2.2f %2.2f \n%2.2f %2.2f %2.2f \n%2.2f %2.2f %2.2f",
                  vol.Val(o[0]-1,o[1]+1,o[2]), vol.Val(o[0],o[1]+1,o[2]), vol.Val(o[0]+1,o[1]+1,o[2]),
                  vol.Val(o[0]-1,o[1],o[2]),   vol.Val(o[0],o[1],o[2]), vol.Val(o[0]+1,o[1]+0,o[2]),
                  vol.Val(o[0]-1,o[1]-1,o[2]), vol.Val(o[0],o[1]-1,o[2]), vol.Val(o[0]+1,o[1]-1,o[2]) );
        qDebug() << q;
        qDebug();
        o = off; // use the gridaccell data
        q.sprintf("%2.2f %2.2f %2.2f \n%2.2f %2.2f %2.2f \n%2.2f %2.2f %2.2f",
                  vol.Val(o[0]-1,o[1]+1,o[2]), vol.Val(o[0],o[1]+1,o[2]), vol.Val(o[0]+1,o[1]+1,o[2]),
                  vol.Val(o[0]-1,o[1],o[2]),   vol.Val(o[0],o[1],o[2]), vol.Val(o[0]+1,o[1]+0,o[2]),
                  vol.Val(o[0]-1,o[1]-1,o[2]), vol.Val(o[0],o[1]-1,o[2]), vol.Val(o[0]+1,o[1]-1,o[2]) );
        qDebug() << q;
    }
#endif

    glEnable( GL_LIGHTING );
}
void Balloon::render_surf_to_vol(){
    if( !vol.isInit() ) return;
    Point3f p, proj;
    glDisable(GL_LIGHTING);
    for(int i=0;i<vol.size(0);i++)
        for(int j=0;j<vol.size(1);j++)
            for(int k=0;k<vol.size(2);k++){
                // if( i!=3 || j!=6 || k!=5 ) continue;

                MyVoxel& v = vol.Voxel(i,j,k);
                vol.off2pos(i,j,k,p);
                // Only ones belonging to active band
                if( v.status ==  2 ){ // && ( tri::Index(surf, v.face)==35||tri::Index(surf, v.face)==34) ){
                    assert( v.face != 0 );
                    vcg::drawBox(p, .05*vol.getDelta());
                    Point3f proj;
                    float dist = vcg::SignedFacePointDistance(*v.face, p, proj);
                    //proj = vcg::FaceCentroid(*v.face);
                    vcg::drawSegment( p, proj );
                }
            }
    glEnable(GL_LIGHTING);
}
void Balloon::render(GLArea* gla){
    if( rm & SHOW_CLOUD )
        render_cloud();
    if( rm & SHOW_VOLUME )
        vol.render();
    if( rm & SHOW_SURF )
        render_isosurface(gla);
    //if( rm & SHOW_ACCEL )
    //    gridAccell.render();
    if( rm & SHOW_3DDDR )
        render_surf_to_acc();
    if( rm & SHOW_SURF_TO_VOL )
        render_surf_to_vol();
}
