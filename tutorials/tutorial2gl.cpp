// STL includes
#include <cstdlib>
#include <cstdio>
#include <random>
#include <utility>
#include <string>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <thread>

std::default_random_engine gen;
std::uniform_real_distribution<double> dist(0,1);

// Local includes
#define _USE_MATH_DEFINES
#include "common.hpp"
#include "opengl.hpp"

// Scene description
Material phongH(Vector(), Vector(), Vector(1,1,1)*.999, 1.E3);
Material phongL(Vector(), Vector(), Vector(1,1,1)*.999, 1.E2);

std::vector<Sphere> spheres = {
   Sphere(Vector(27,16.5,47),        16.5, phongH),//RightSp
   Sphere(Vector(73,16.5,78),        16.5, phongL),//LeftSp
   Sphere(Vector( 1e5+1,40.8,81.6),  1e5,  Vector(),Vector(.75,.25,.25)),//Left
   Sphere(Vector(-1e5+99,40.8,81.6), 1e5,  Vector(),Vector(.25,.25,.75)),//Rght
   Sphere(Vector(50,40.8, 1e5),      1e5,  Vector(),Vector(.75,.75,.75)),//Back
   Sphere(Vector(50,40.8,-1e5+170),  1e5,  Vector(),Vector()           ),//Frnt
   Sphere(Vector(50, 1e5, 81.6),     1e5,  Vector(),Vector(.75,.75,.75)),//Botm
   Sphere(Vector(50,-1e5+81.6,81.6), 1e5,  Vector(),Vector(.75,.75,.75)),//Top
   Sphere(Vector(50,681.6-.27,81.6), 600,  Vector(12,12,12),  Vector()) //Lite
};

// Texture for the bcg_img + size
int   width = 512, height = 512;
bool  generateBackground = true;
bool  displayBackground  = true;
bool  generateCovariance = true;
bool  useCovFilter       = true;
bool  generateReference  = false;
int   nPasses            = 0;
int   nPassesFilter      = 0;
float filterRadius       = 1.0f;

ShaderProgram* program;

// Different buffer, the background image, the covariance filter and the brute
// force filter.
GLuint texs_id[3];
float* bcg_img = new float[width*height]; float bcg_scale = 1.0f;
float* cov_img = new float[width*height]; float cov_scale = 1.0f;
float* ref_img = new float[width*height]; float ref_scale = 1.0f;

// Camera frame
Ray cam(Vector(50,52,295.6), Vector(0,-0.042612,-1).Normalize());
double fov  = 1.2;
Vector  cx  = Vector(width*fov/height);
Vector  cy  = Vector::Cross(cx, cam.d).Normalize()*fov;
Vector ncx  = cx;
Vector ncy  = cy;

std::stringstream sout;

void ExportImage() {
   Vector* img = new Vector[width*height];
   int x = width*mouse.X, y = height*mouse.Y;
   #pragma omp parallel for schedule(dynamic, 1)
   for(int i=0; i<width; ++i) {
      for(int j=0; j<height; ++j) {
         int id = i + j*width;
         int di = i + (height-j)*width;

         bool filter = ((abs(j-x)-4 < 0 && i==y) || (abs(i-y)-4 < 0 && j==x)) &&
                       (generateReference || generateCovariance);

         // Background img
         img[id] = (displayBackground) ? bcg_img[di]*Vector(1,1,1) : Vector(0,0,0);

         // Adding the filters
         img[id].x += generateCovariance ? cov_scale*cov_img[di] : 0.0f;
         img[id].y += generateReference  ? ref_scale*ref_img[di] : 0.0f;
         img[id].z += (filter ? 1.0f : 0.0f);
      }
   }

   int ret = SaveEXR(img, width, height, "output.exr");
   if(ret != 0) { std::cerr << "Unable to export image" << std::endl; }
}

void KeyboardKeys(unsigned char key, int x, int y) {
   if(key == 'c') {
      memset(cov_img, 0.0, width*height);
      generateCovariance = !generateCovariance;
   } else if(key == 'B') {
      generateReference = !generateReference;
   } else if(key == 'b') {
      generateBackground = !generateBackground;
   } else if(key == 'h') {
      displayBackground  = !displayBackground;
   } else if(key == 'f') {
       useCovFilter = !useCovFilter;
   } else if(key == '+') {
      Material phong(Vector(), Vector(), Vector(1,1,1)*.999, spheres[1].mat.exponent * 10);
      spheres[1].mat = phong;
      nPasses = 0;
   } else if(key == '-') {
      Material phong(Vector(), Vector(), Vector(1,1,1)*.999, fmax(spheres[1].mat.exponent / 10, 1.0));
      spheres[1].mat = phong;
      nPasses = 0;
   } else if(key == 'p') {
      ExportImage();
   } else if(key == 'd') {
      std::cout << sout.str() << std::endl;
   }
   glutPostRedisplay();
}

void RadianceTexture() {

   // Loop over the rows and columns of the image and evaluate radiance and
   // covariance per pixel using Monte-Carlo.
   #pragma omp parallel for schedule(dynamic, 1)
   for (int y=0; y<height; y++){
      Random rng(y + height*clock());

      for (int x=0; x<width; x++) {
         int i=(width-x-1)*height+y;

         // Create the RNG and get the sub-pixel sample
         float dx = rng();
         float dy = rng();

         // Generate the pixel direction
         Vector d = cx*((dx + x)/float(width)  - .5) +
                    cy*((dy + y)/float(height) - .5) + cam.d;
         d.Normalize();

         Ray ray(cam.o, d);
         Vector radiance = Radiance(spheres, ray, rng, 0, 1);

         bcg_img[i] = (float(nPasses)*bcg_img[i] + Vector::Dot(radiance, Vector(1,1,1))/3.0f) / float(nPasses+1);
      }
   }

   ++nPasses;
   glutPostRedisplay();
}

PosCov CovarianceFilter(const std::vector<Sphere>& spheres, const Ray &r,
                        const Cov4D& cov, int depth, int maxdepth,
                        std::stringstream& out) {
   double t;
   int id=0;
   if (!Intersect(spheres, r, t, id)) {
     return PosCov(Vector(), Cov4D());
   }
   const Sphere&   obj = spheres[id];
   const Material& mat = obj.mat;
   Vector x  = r.o+r.d*t,
          n  = (x-obj.c).Normalize(),
          nl = Vector::Dot(n,r.d) < 0 ? n:n*-1;
   const double k = 1.f/spheres[id].r;

   // Update the covariance with travel and project it onto the tangent plane
   // of the hit object.
   Cov4D cov2 = cov;
   cov2.Travel(t);
   out << "After travel of " << t << " meters" << std::endl;
   out << cov2 << std::endl;
   out << "Volume = " << cov2.Volume() << std::endl;
   out << std::endl;

   out << "After projection, cos=" << Vector::Dot(n, cov2.z) << std::endl;
   cov2.Projection(n);
   out << cov2 << std::endl;
   out << "Volume = " << cov2.Volume() << std::endl;
   out << std::endl;

   // if the max depth is reached
   if(depth >= maxdepth) {
      return PosCov(x, cov2);
   } else {
      // Sample a new direction
      auto wi = -r.d;
      auto wr = 2*Vector::Dot(wi, nl)*nl - wi;
      auto r2 = Ray(x, wr);

      cov2.Curvature(k, k);
      out << "After curvature" << std::endl;
      out << cov2 << std::endl;
      out << "Volume = " << cov2.Volume() << std::endl;
      out << std::endl;

      cov2.Cosine(1.0f);
      out << "After cosine multiplication" << std::endl;
      out << cov2 << std::endl;

      cov2.Symmetry();
      out << "After symmetry" << std::endl;
      out << cov2 << std::endl;
      out << "Volume = " << cov2.Volume() << std::endl;
      out << std::endl;

      const double rho = mat.exponent / (4*M_PI*M_PI);
      cov2.Reflection(rho, rho);
      out << "After BRDF convolution, sigma=" << rho << std::endl;
      out << cov2 << std::endl;
      out << "Volume = " << cov2.Volume() << std::endl;
      out << std::endl;

      cov2.Curvature(-k, -k);
      out << "After inverse curvature, k=" << -k << std::endl;
      out << cov2 << std::endl;
      out << "Volume = " << cov2.Volume() << std::endl;
      out << std::endl;

      cov2.InverseProjection(wr);
      out << "After inverse projection, cos=" << Vector::Dot(n, wr) << std::endl;
      out << cov2 << std::endl;
      out << "Volume = " << cov2.Volume() << std::endl;
      out << std::endl;

      return CovarianceFilter(spheres, r2, cov2, depth+1, maxdepth, out);
   }
}

void CovarianceTexture() {
   // Generate a covariance matrix at the sampling position
   int x = width*mouse.X, y = height*mouse.Y;
   const auto t = (cx*((x+0.5)/double(width) - .5) + cy*((y+0.5)/double(height) - .5) + cam.d).Normalize();
   //*/
   const auto pixelCov = Cov4D({ 1.0E+5, 0.0, 1.0E+5, 0.0, 0.0, 1.0E+5, 0.0, 0.0, 0.0, 1.0E+5 }, t);
   /*/
   const auto pixelCov = Cov4D({ 1.0E-5, 0.0, 1.0E-5, 0.0, 0.0, 1.0E-5, 0.0, 0.0, 0.0, 1.0E-5 }, t);
   //*/
   sout.str("");
   const auto surfCov  = CovarianceFilter(spheres, Ray(cam.o, t), pixelCov, 0, 1, sout);
   sout << surfCov.second << std::endl;
   sout << "Volume = " << surfCov.second.Volume() << std::endl;
   sout << std::endl;

   double sxx = 0, syy = 0, sxy = 0;
   Vector Dx, Dy;
   try {
      surfCov.second.SpatialFilter(sxx, sxy, syy);
      surfCov.second.SpatialExtent(Dx, Dy);
      sout << "Spatial filter = [" << sxx << "," << sxy << "; " << sxy << ", " << syy << "]"<< std::endl;
      sout << "Extent = " << Dx << ", " << Dy << std::endl;
      sout << "|Dx| = " << Vector::Norm(Dx) << ", |Dy| = " << Vector::Norm(Dy) << std::endl;
   } catch (...) {
      std::cout << "Error: incorrect spatial filter" << std::endl;
      sout << surfCov.second << std::endl;
      return;
   }

   // Loop over the rows and columns of the image and evaluate radiance and
   // covariance per pixel using Monte-Carlo.
   #pragma omp parallel for schedule(dynamic, 1)
   for (int y=0; y<height; y++){
      for (int x=0; x<width; x++) {
         // Pixel index
         int i=(width-x-1)*height+y;

         // Generate the pixel direction
         Vector d = cx*( ( 0.5 + x)/width - .5) +
                    cy*( ( 0.5 + y)/height - .5) + cam.d;
         d.Normalize();

         Ray ray(cam.o, d);
         double t; int id;
         if(!Intersect(spheres, ray, t, id)){ continue; }
         Vector hitp = ray.o + t*ray.d;

         // Evaluate the covariance
         const Vector dx  = surfCov.first - hitp;
         const Vector dU = Vector(Vector::Dot(dx, surfCov.second.x), Vector::Dot(dx, surfCov.second.y), Vector::Dot(dx, surfCov.second.z));
         if(useCovFilter) {
            double bf = dU.x*dU.x*sxx + dU.y*dU.y*syy + 2*dU.x*dU.y*sxy;
            cov_img[i] = exp(-10.0*dU.z*dU.z) * exp(- 0.5* bf);
         } else {

            const double du  = Vector::Dot(dU, Dx) / Vector::Norm(Dx);
            const double dv  = Vector::Dot(dU, Dy) / Vector::Norm(Dy);
            cov_img[i] = (abs(du) < Vector::Norm(Dx) &&
                          abs(dv) < Vector::Norm(Dy)) ? exp(-10.0*dU.z*dU.z) : 0.0;
         }
      }
   }
}

using PosFilter = std::pair<Vector, Vector>;

PosFilter indirect_filter(const Ray &r, Random& rng, int depth, int maxdepth=2){
   double t;                               // distance to Intersection
   int id=0;                               // id of Intersected object
   if (!Intersect(spheres, r, t, id)) return PosFilter(Vector(), Vector()); // if miss, return black
   const Sphere&   obj = spheres[id];      // the hit object
   const Material& mat = obj.mat;          // Its material
   Vector x  = r.o+r.d*t,
          n  = (x-obj.c).Normalize(),
          nl = Vector::Dot(n,r.d) < 0 ? n:n*-1;

   // Once you reach the max depth, return the hit position and the filter's
   // value using the recursive form.
   if(depth >= maxdepth) {
      return PosFilter(x, Vector(1.0f, 1.0f, 1.0f));

   // Main covariance computation. First this code generate a new direction
   // and query the covariance+radiance in that direction. Then, it computes
   // the covariance after the reflection/refraction.
   } else {
      /* Sampling a new direction + recursive call */
      double pdf;
      const auto e   = Vector(rng(), rng(), rng());
      const auto wo  = -r.d;
      const auto wi  = mat.Sample(wo, nl, e, pdf);
            auto f   = Vector::Dot(wi, nl)*mat.Reflectance(wi, wo, nl);
      const auto res = indirect_filter(Ray(x, wi), rng, depth+1, maxdepth);
      return PosFilter(res.first, (1.f/pdf) * f.Multiply(res.second));
   }
}

void BruteForceTexture(int samps = 1000) {

   std::vector<PosFilter> _filter_elems;

   // Check pixel position and clean the filter if there was any motion of the
   // mouse.
   int x = width*mouse.X, y = height*mouse.Y;
   if(fabs(mouse.Dx) > 0.0f || fabs(mouse.Dy) > 0.0f) {
      nPassesFilter = 0;
      filterRadius  = 1.0f;
   }

   // Sub pixel sampling
   const int nthread = std::thread::hardware_concurrency();
   #pragma omp parallel for schedule(dynamic, 1)
   for(int t=0; t<nthread; ++t) {
      Random rng(t + nthread*clock());

      for(int s=0; s<samps/nthread; s++){

         // Create the RNG and get the sub-pixel sample
         float dx = rng();
         float dy = rng();

         // Generate the pixel direction
         Vector d = cx*((dx + x)/width  - .5) +
            cy*((dy + y)/height - .5) + cam.d;
         d.Normalize();

         // Evaluate the Covariance and Radiance at the pixel location
         const auto filter = indirect_filter(Ray(cam.o, d), rng, 0, 1);
         #pragma omp critical
         {
            _filter_elems.push_back(filter);
         }
      }
   }

   // Loop over the rows and columns of the image and evaluate radiance and
   // covariance per pixel using Monte-Carlo.
   float max_ref = 0.0f;
   #pragma omp parallel for schedule(dynamic, 1), shared(max_ref)
   for (int y=0; y<height; y++){
      float max_temp = 0.0f;

      for (int x=0; x<width; x++) {
         int i=(width-x-1)*height+y;
         float _r = 0.0f;

         // Generate the pixel direction
         Vector d = cx*((0.5 + x)/width  - .5) +
                    cy*((0.5 + y)/height - .5) + cam.d;
         d.Normalize();

         Ray ray(cam.o, d);
         double t; int id;
         if(!Intersect(spheres, ray, t, id)){ continue; }
         Vector hitp = ray.o + t*ray.d;

         for(auto& elem : _filter_elems) {
            const auto& p = elem.first;
            const auto  x = Vector::Norm(hitp-p) / filterRadius;
            _r += (0.3989422804f/filterRadius) * exp(-0.5f * pow(x, 2)) * elem.second.x;
         }

         const auto scale = 20.f;
         const auto Nold  = nPassesFilter * samps;
         const auto Nnew  = (nPassesFilter+1) * samps;
         ref_img[i] = (ref_img[i]*Nold + scale*_r) / float(Nnew);
         max_temp   = std::max(ref_img[i], max_temp);
      }

      #pragma omp critical
      {
         max_ref = std::max(max_ref, max_temp);
      }
   }

   // Update the scaling
   ref_scale = 1.0f/max_ref;

   // Progressive refinement of the radius and number of passes
   filterRadius *= sqrt((nPassesFilter + 0.8) / (nPassesFilter + 1.0));
   ++nPassesFilter;

   glutPostRedisplay();
}

void Draw() {

   if(generateBackground) {
      RadianceTexture();

      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, texs_id[0]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_LUMINANCE, GL_FLOAT, bcg_img);
   }

   if(generateCovariance) {
      CovarianceTexture();

      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, texs_id[1]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_LUMINANCE, GL_FLOAT, cov_img);
   }

   if(generateReference) {
      BruteForceTexture();

      glActiveTexture(GL_TEXTURE2);
      glBindTexture(GL_TEXTURE_2D, texs_id[2]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_LUMINANCE, GL_FLOAT, ref_img);
   }

   program->use();

   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, texs_id[0]);

   glActiveTexture(GL_TEXTURE1);
   glBindTexture(GL_TEXTURE_2D, texs_id[1]);

   glActiveTexture(GL_TEXTURE2);
   glBindTexture(GL_TEXTURE_2D, texs_id[2]);

   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
   auto uniLocation = program->uniform("pointer");
   glUniform2f(uniLocation, mouse.Y, 1.0-mouse.X);

   // Update the scaling
   glUniform1f(program->uniform("tex0scale"), displayBackground  ? bcg_scale : 0.0f);
   glUniform1f(program->uniform("tex1scale"), generateCovariance ? cov_scale : 0.0f);
   glUniform1f(program->uniform("tex2scale"), generateReference  ? ref_scale : 0.0f);

   glBegin(GL_QUADS);
   glVertex3f(-1.0f,-1.0f, 0.0f); glTexCoord2f(0, 0);
   glVertex3f( 1.0f,-1.0f, 0.0f); glTexCoord2f(1, 0);
   glVertex3f( 1.0f, 1.0f, 0.0f); glTexCoord2f(1, 1);
   glVertex3f(-1.0f, 1.0f, 0.0f); glTexCoord2f(0, 1);
   glEnd();
   program->disable();
   glutSwapBuffers();
}

// Create geometry and textures
void Init() {
   // Background color
   glClearColor(0.0f, 0.0f, 0.0f, 2.0f);

   // Create the shader programs
   program = new ShaderProgram(false);
   std::string vertShader =
      "void main(void) {"
      "   gl_TexCoord[0] = gl_MultiTexCoord0;"
      "   gl_Position    = vec4(gl_Vertex);"
      "}";
   std::string fragShader =
      "uniform sampler2D tex0; uniform float tex0scale;"
      "uniform sampler2D tex1; uniform float tex1scale;"
      "uniform sampler2D tex2; uniform float tex2scale;"
      "uniform vec2      pointer;"
      "uniform float     width;"
      "uniform float     height;"
      "void main(void) {"
      "  float fact = exp(- width*height * pow(length(gl_TexCoord[0].xy - pointer.xy), 2.0));"
      "  gl_FragColor = vec4(0,0,1,1)*fact + tex0scale*vec4(1,1,1,1)*texture2D(tex0, gl_TexCoord[0].st) + tex1scale*vec4(1,0,0,1)*texture2D(tex1, gl_TexCoord[0].st) + tex2scale*vec4(0,1,0,1)*texture2D(tex2, gl_TexCoord[0].st);"
      "}";
   program->initFromStrings(vertShader, fragShader);

   // Reserve textures on the GPU
   glGenTextures(3, texs_id);

   // Define the different uniform locations in the shader
   program->use();

   const auto t1Location = program->addUniform("tex0");
   glUniform1i(t1Location, 0);
   const auto t2Location = program->addUniform("tex1");
   glUniform1i(t2Location, 1);
   const auto t3Location = program->addUniform("tex2");
   glUniform1i(t3Location, 2);

   const auto t1sLocation = program->addUniform("tex0scale");
   glUniform1f(t1sLocation, bcg_scale);
   const auto t2sLocation = program->addUniform("tex1scale");
   glUniform1f(t2sLocation, cov_scale);
   const auto t3sLocation = program->addUniform("tex2scale");
   glUniform1f(t3sLocation, ref_scale);

   const auto uniWidth = program->addUniform("width");
   glUniform1f(uniWidth, float(width));
   const auto uniHeight = program->addUniform("height");
   glUniform1f(uniHeight, float(height));

   const auto uniLocation = program->addUniform("pointer");
   glUniform2f(uniLocation, width*mouse.X, height*mouse.Y);

   program->disable();

   // Clean buffer memory
   memset(ref_img, 0.0, width*height);
   memset(cov_img, 0.0, width*height);
}

void PrintHelp() {
   std::cout << "Covariance Tracing tutorial 2" << std::endl;
   std::cout << "----------------------------" << std::endl;
   std::cout << std::endl;
   std::cout << "This tutorial display the indirect pixel filter after one bounce for" << std::endl;
   std::cout << "non-diffuse surfaces. To display the filter, click on one of the two" << std::endl;
   std::cout << "shiny spheres." << std::endl;
   std::cout << std::endl;
   std::cout << " + 'b' stop/resume rendering the background image" << std::endl;
   std::cout << " + 'h' hide/show the background image" << std::endl;
   std::cout << " + 'c' stop/resume rendering the covariance filter" << std::endl;
   std::cout << " + 'B' brute-force indirect pixel filter (SLOW)" << std::endl;
   std::cout << " + 'p' output image to EXR file" << std::endl;
   std::cout << " + 'd' print Covariance Tracing step by step" << std::endl;
   std::cout << " + 'f' switch between displaying the Gaussian filter or the polygonal footprint" << std::endl;
   std::cout << " + '+' increase the BRDF exponent for the right sphere" << std::endl;
   std::cout << " + '-' decrease the BRDF exponent for the right sphere" << std::endl;
   std::cout << std::endl;
}

int main(int argc, char** argv) {
   cam.o = cam.o + 140.0*cam.d;
   ncx.Normalize();
   ncy.Normalize();

   mouse.X = 0.5;
   mouse.Y = 0.5;

   PrintHelp();

   glutInit(&argc, argv);
   glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE | GLUT_DEPTH);

   glutInitWindowSize(width, height);
   glutCreateWindow("Covariance Tracing tutorial 2");

   Init();

   glutDisplayFunc(Draw);
   glutMouseFunc(MouseClicked);
   glutMotionFunc(MouseMoved);
   glutKeyboardFunc(KeyboardKeys);
   glutMainLoop();

   if(bcg_img) { delete[] bcg_img; }
   if(cov_img) { delete[] cov_img; }
   if(ref_img) { delete[] ref_img; }
   return EXIT_SUCCESS;
}
