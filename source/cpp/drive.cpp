#include <iostream>
#include <sys/time.h>
#include <opencv/cv.h>
#include <opencv/highgui.h>
#include "drive.h"

using namespace std;
using Eigen::Matrix2f;
using Eigen::Matrix3f;
using Eigen::Matrix4f;
using Eigen::MatrixXf;
using Eigen::Vector2f;
using Eigen::Vector3f;
using Eigen::VectorXf;

// Our output throttle value
int8_t throttle_ = 0;

// Our output steering angle
int8_t steering_ = 0;

// The actual current angle of steering (as servos aren't instant)
uint8_t servo_pos_ = 110;

// The current IMU values
Eigen::Vector3f accel_(0, 0, 0), gyro_(0, 0, 0);

// The four wheel encoder values from the four wheels
uint16_t wheel_pos_[4] = {0, 0, 0, 0};
uint16_t last_encoders_[4];

// Time keeping
struct timeval t;
struct timeval _last_t;

// Keyboard key presses
char key;

static const float MAX_THROTTLE = 0.8;
static const float SPEED_LIMIT = 5.0;

static const float ACCEL_LIMIT = 4.0;     // Maximum dv/dt (m/s^2)
static const float BRAKE_LIMIT = -100.0;  // Minimum dv/dt
static const float TRACTION_LIMIT = 4.0;  // Maximum v*w product (m/s^2)
static const float kpy = 1.0;
static const float kvy = 2.0;

static const float LANE_OFFSET = 0.0;
static const float LANEOFFSET_PER_K = 0.0;

Drive::Drive() {
  Reset();
}

void Drive::Reset() {
  kalman_filter.Reset();
  _first_frame = true;
}

void Drive::Update(float throttle, float steering, float dt) {

  // Get our current model of the world
  Eigen::VectorXf &_x = kalman_filter.GetState();

  // Freak out if we freaked out
  if(isinf(_x[0]) || isnan(_x[0])) {
    fprintf(stderr, "Caution: Massive freakout currently underweigh.\n");
    Reset();
    return;
  }

  // Start off sensibly
  if(_first_frame) {
    _first_frame = false;
  }

  // After 'dt' milliseconds since we last thought about it, what would we expect the world to look like now? 
  kalman_filter.Predict(dt, throttle, steering);

  // Log
  std::cout << "World state after our prediction: " << _x.transpose() << std::endl;

}

static inline float clip(float x, float min, float max) {
  if (x < min) return min;
  if (x > max) return max;
  return x;
}



static const int ytop = 100;
// uxrange (-56, 55) uyrange (2, 59) x0 -56 y0 2
static const int ux0 = -56, uy0 = 2;

// 30 for home, 15 for diyrobocars shiny track
static const int ACTIV_THRESH = 30;

static const int uxsiz = 111, uysiz = 57;

static const float pixel_scale_m = 0.025;

// (57, 111), (3197, 2)
static const uint16_t bucketcount[uxsiz * uysiz] = {
#include "bucketcount.txt"
};

static const uint16_t floodmap[uxsiz * uysiz] = {
#include "floodmap.txt"
};

static const uint8_t udmask[320*(240-ytop)] = {
#include "udmask.txt"
};

static const int8_t udplane[320*(240-ytop)*2] = {
#include "udplane.txt"
};

bool TophatFilter(const uint8_t *yuv, Vector3f *Bout,
    float *y_cout, Matrix4f *Rkout) {
  int32_t accumbuf[uxsiz * uysiz * 3];
  // input is a 640x480 YUV420 image
  memset(accumbuf, 0, uxsiz * uysiz * 3 * sizeof(accumbuf[0]));

  static int snapshot = 0;

  FILE *fp = NULL;
  snapshot++;
  if (snapshot == 40) {
    fp = fopen("snapshot.bin", "w");
  }
  // for each yuv, (maybe) remap into detected
  size_t bufidx = ytop*320;
  size_t udidx = 0;
  for (int j = 0; j < 240 - ytop; j++) {
    for (int i = 0; i < 320; i++, bufidx++, udidx++) {
      uint8_t y = yuv[(j+ytop)*2*640 + 2*i];
      uint8_t u = yuv[640*480 + bufidx];
      uint8_t v = yuv[640*480 + 320*240 + bufidx];

      if (fp) {
        fwrite(&y, 1, 1, fp);
        fwrite(&u, 1, 1, fp);
        fwrite(&v, 1, 1, fp);
      }

      if (!udmask[udidx]) continue;
      int dx = udplane[udidx*2] - ux0;
      int dy = udplane[udidx*2 + 1] - uy0;

      accumbuf[(uxsiz * dy + dx) * 3] += y;
      accumbuf[(uxsiz * dy + dx) * 3 + 1] += u;
      accumbuf[(uxsiz * dy + dx) * 3 + 2] += v;
    }
  }

  size_t uidx = 0;
  // average
  for (int j = 0; j < uysiz; j++) {
    for (int i = 0; i < uxsiz; uidx++, i++) {
      if (bucketcount[uidx] > 0) {
        accumbuf[uidx*3] /= bucketcount[uidx];
        accumbuf[uidx*3 + 1] /= bucketcount[uidx];
        accumbuf[uidx*3 + 2] /= bucketcount[uidx];
      }
      if (fp) {
        fwrite(&accumbuf[uidx*3], 4, 3, fp);
      }
    }
  }

  // flood-fill
  uidx = 0;
  for (int j = 0; j < uysiz; j++) {
    for (int i = 0; i < uxsiz; uidx++, i++) {
      if (bucketcount[uidx] == 0) {
        accumbuf[uidx*3] = accumbuf[floodmap[uidx]*3];
        accumbuf[uidx*3 + 1] = accumbuf[floodmap[uidx]*3 + 1];
        accumbuf[uidx*3 + 2] = accumbuf[floodmap[uidx]*3 + 2];
      }
      if (fp) {
        fwrite(&accumbuf[uidx*3], 4, 3, fp);
      }
    }
  }

  // horizontal cumsum
  for (int j = 0; j < uysiz; j++) {
    for (int i = 1; i < uxsiz; i++) {
      accumbuf[3*(j*uxsiz + i)] += accumbuf[3*(j*uxsiz + i - 1)];
      accumbuf[3*(j*uxsiz + i) + 1] += accumbuf[3*(j*uxsiz + i - 1) + 1];
      accumbuf[3*(j*uxsiz + i) + 2] += accumbuf[3*(j*uxsiz + i - 1) + 2];
    }
  }

  // horizontal convolution w/ [-1, -1, 2, 2, -1, -1]
  Matrix3f regXTX = Matrix3f::Zero();
  Vector3f regXTy = Vector3f::Zero();
  double regyTy = 0;
  double regxsum = 0;
  double regwsum = 0;
  int regN = 0;

  for (int j = 0; j < uysiz; j++) {
    for (int i = 0; i < uxsiz-7; i++) {
      int32_t yd =
        -(accumbuf[3*(j*uxsiz + i + 6)] - accumbuf[3*(j*uxsiz + i)])
        + 3*(accumbuf[3*(j*uxsiz + i + 4)] - accumbuf[3*(j*uxsiz + i + 2)]);
      int32_t ud =
        -(accumbuf[3*(j*uxsiz + i + 6) + 1] - accumbuf[3*(j*uxsiz + i) + 1])
        + 3*(accumbuf[3*(j*uxsiz + i + 4) + 1] - accumbuf[3*(j*uxsiz + i + 2) + 1]);
      int32_t vd =
        -(accumbuf[3*(j*uxsiz + i + 6) + 2] - accumbuf[3*(j*uxsiz + i) + 2])
        + 3*(accumbuf[3*(j*uxsiz + i + 4) + 2] - accumbuf[3*(j*uxsiz + i + 2) + 2]);

      // detected = (0.25*hv[:, :, 0] - 2*hv[:, :, 1] + 0.5*hv[:, :, 2] - 30)
      //int32_t detected = (yd >> 2) - (ud << 1) + (vd >> 1) - 60;
      int32_t detected = -ud - ACTIV_THRESH;
      if (fp) {
        fwrite(&yd, 4, 1, fp);
        fwrite(&ud, 4, 1, fp);
        fwrite(&vd, 4, 1, fp);
      }
      if (detected > 0) {
        // add x, y to linear regression
        float pu = pixel_scale_m * (i + ux0 + 3),
              pv = pixel_scale_m * (j + uy0);
        float w = detected;  // use activation as regression weight
        Vector3f regX(w*pv*pv, w*pv, w);
        regxsum += w*pv;
        regwsum += w;
        regXTX.noalias() += regX * regX.transpose();
        regXTy.noalias() += regX * w * pu;
        regyTy += w * w * pu * pu;
        regN += 1;
      }
    }
  }
  if (fp) {
    fclose(fp);
    fp = NULL;
  }

  std::cout << "activations " << regN << "\n";

  // not enough data, don't even try to do an update
  if (regN < 8) {
    return false;
  }

  Matrix3f XTXinv = regXTX.inverse();
  Vector3f B = XTXinv * regXTy;
  *Bout = B;

  // (XB).T y
  // BT XTy
  float r2 = B.dot(regXTX * B) - 2*B.dot(regXTy) + regyTy;
  r2 *= 100.0 / (regN - 1);

  *y_cout = regxsum / regwsum;
#if 0
  std::cout << "XTX\n" << regXTX << "\n";
  std::cout << "XTy " << regXTy.transpose() << "\n";
  std::cout << "yTy " << regyTy << "\n";
  std::cout << "XTXinv\n" << XTXinv << "\n";
#endif
#if 1
  std::cout << "B " << B.transpose() << "\n";
  std::cout << "r2 " << r2 << "\n";
  std::cout << "y_c " << *y_cout << "\n";
#endif

  if (isnan(r2)) {
    return false;
  }

  (*Rkout).topLeftCorner(3, 3) = XTXinv * r2;
  (*Rkout)(3, 3) = regXTX(1, 1) / regwsum - *y_cout;
  return true;
}

void Drive::UpdateCamera(const uint8_t *frame) {
  Vector3f B;
  Matrix4f Rk = Matrix4f::Zero();
  float yc;

  // Look for that line
  if (!TophatFilter(frame, &B, &yc, &Rk)) {
    return;
  }

  // We've got our linear fit B[0:2] and our measurement covariance Rk, time to do the sensor fusion step
  kalman_filter.UpdateCenterline(B[0], B[1], B[2], yc, Rk);
}

void Drive::UpdateState(const uint8_t *yuv, size_t yuvlen,
      float throttle_in, float steering_in,
      const Vector3f &accel, const Vector3f &gyro,
      uint8_t servo_pos, const uint16_t *wheel_encoders, float dt) {
  Eigen::VectorXf &x_ = kalman_filter.GetState();

  if (isinf(x_[0]) || isnan(x_[0])) {
    fprintf(stderr, "WARNING: kalman filter diverged to inf/NaN! resetting!\n");
    Reset();
    return;
  }

  if (_first_frame) {
    memcpy(last_encoders_, wheel_encoders, 4*sizeof(uint16_t));
    _first_frame = false;
  }

  kalman_filter.Predict(dt, throttle_in, steering_in);
  std::cout << "x after predict " << x_.transpose() << std::endl;

  if (yuvlen == 640*480 + 320*240*2) {
    UpdateCamera(yuv);
    std::cout << "x after camera " << x_.transpose() << std::endl;
  } else {
    fprintf(stderr, "DriveController::UpdateState: invalid yuvlen %ld, expected %d\n",
        yuvlen, 320*240*2);
  }

  kalman_filter.UpdateIMU(gyro[2]);
  std::cout << "x after IMU (" << gyro[2] << ")" << x_.transpose() << std::endl;

  // Force psi_e to be forward facing
  if (x_[3] > M_PI/2) {
    x_[3] -= M_PI;
  } else if (x_[3] < -M_PI/2) {
    x_[3] += M_PI;
  }

  // read / update servo & encoders
  // use the average of the two rear encoders as we're most interested in the
  // motor speed
  // but we could use all four to get turning radius, etc.
  // since the encoders are just 16-bit counters which wrap frequently, we only
  // track the difference in counts between updates.
  printf("encoders were: %05d %05d %05d %05d\n"
         "      are now: %05d %05d %05d %05d\n",
      last_encoders_[0], last_encoders_[1], last_encoders_[2], last_encoders_[3],
      wheel_encoders[0], wheel_encoders[1], wheel_encoders[2], wheel_encoders[3]);

  // average ds among wheel encoders which are actually moving
  float ds = 0, nds = 0;
  for (int i = 2; i < 4; i++) {  // only use rear encoders
    if (wheel_encoders[i] != last_encoders_[i]) {
      ds += (uint16_t) (wheel_encoders[i] - last_encoders_[i]);
      nds += 1;
    }
  }
  memcpy(last_encoders_, wheel_encoders, 4*sizeof(uint16_t));

  // and do an kalman_filter update if the wheels are moving.
  if (nds > 0) {
    kalman_filter.UpdateEncoders(ds/(nds * dt), servo_pos);
    std::cout << "x after encoders (" << ds/dt << ") " << x_.transpose() << std::endl;
  } else {
    kalman_filter.UpdateEncoders(0, servo_pos);
    std::cout << "x after encoders (" << ds/dt << ") " << x_.transpose() << std::endl;
  }

  std::cout << "P " << kalman_filter.GetCovariance().diagonal().transpose() << std::endl;
}

static float MotorControl(float accel, float k1, float k2, float k3, float k4, float v) {
  float a_thresh = -k3 * v - k4;

  // voltage (1 or 0)
  float V = accel > a_thresh ? 1 : 0;

  // duty cycle
  float DC = clip((accel + k3*v + k4) / (V*k1 - k2*v), 0, 1);
  return V == 1 ? DC : -DC;
}

bool Drive::GetControl(float *throttle_out, float *steering_out, float dt) {
  const Eigen::VectorXf &x_ = kalman_filter.GetState();
  float v = x_[0];
  float delta = x_[1];
  float y_e = x_[2];
  float psi_e = x_[3];
  float kappa = x_[4];
  float ml_1 = x_[5];
  float ml_2 = x_[6];
  float ml_3 = x_[7];
  float ml_4 = x_[8];
  float srv_a = x_[9];
  float srv_b = x_[10];
  float srv_r = x_[11];

  float k1 = exp(ml_1), k2 = exp(ml_2), k3 = exp(ml_3), k4 = exp(ml_4);

  float vmax = fmin(SPEED_LIMIT, (k1 - k4)/(k2 + k3));

  // TODO: race line following w/ particle filter localization
  float lane_offset = clip(LANE_OFFSET + kappa * LANEOFFSET_PER_K, -1.0, 1.0);
  float psi_offset = 0;

  float cpsi = cos(psi_e - psi_offset),
        spsi = sin(psi_e - psi_offset);
  float dx = cpsi / (1.0 - kappa*y_e);

  // it's a little backwards though because our steering is reversed w.r.t. curvature
  float k_target = dx * (-(y_e - lane_offset) * dx * kpy*cpsi - spsi*(-kappa*spsi - kvy*cpsi) + kappa);

  *steering_out = clip((k_target - srv_b) / srv_a, -1, 1);
  if (*steering_out == -1 || *steering_out == 1) {
    // steering is clamped, so we may need to further limit speed
    float w_target = v * k_target;
    float k_limit = srv_a * (*steering_out) + srv_b;
    vmax = fmin(vmax, w_target / k_limit);
  }

  float v_target = fmin(vmax, sqrtf(TRACTION_LIMIT / fabs(k_target)));
  float a_target = clip(v_target - v, BRAKE_LIMIT, ACCEL_LIMIT) / dt;
  if (a_target > 0) {  // accelerate more gently than braking
    a_target /= 4;
  }
  *throttle_out = clip(MotorControl(a_target, k1, k2, k3, k4, v), -1, MAX_THROTTLE);

  printf("steer_target %f delta %f v_target %f v %f a_target %f lateral_a %f/%f v %f y %f psi %f\n",
      k_target, delta, v_target, v, a_target, v*v*delta, TRACTION_LIMIT, v, y_e, psi_e);

  printf("Throttle: %f, Steering: %f\n", *throttle_out, *steering_out);
  return true;
}


int main(){

  // Start it up
	printf("\nStarting OpenRover.\n");

  // Start up our car driver
  Drive drive;

  // Create window
  cvNamedWindow("Camera_Output", 1);

  // Start cam
  CvCapture* capture = cvCaptureFromCAM(CV_CAP_ANY);
  cvSetCaptureProperty(capture, CV_CAP_PROP_FRAME_WIDTH, 320);
  cvSetCaptureProperty(capture, CV_CAP_PROP_FRAME_HEIGHT, 240);

  while(1){

    // Get frame
    IplImage* frame = cvQueryFrame(capture);

    // Show frame
    cvShowImage("Camera_Output", frame);
    int frame_length = frame->imageSize;

    // Get time
    gettimeofday(&t, NULL);

    uint8_t* f = (unsigned char*)frame->imageData;

    // Update our model of the world
    float u_a = throttle_ / 127.0;
    float u_s = steering_ / 127.0;
    float dt = t.tv_sec - _last_t.tv_sec + (t.tv_usec - _last_t.tv_usec) * 1e-6;
    drive.UpdateState(f, frame_length, u_a, u_s, accel_, gyro_, servo_pos_, wheel_pos_, dt);
    _last_t = t;

    // Get control commands from model
    if(drive.GetControl(&u_a, &u_s, dt)) {
      steering_ = 127 * u_s;
      throttle_ = 127 * u_a;
      printf("Throttle: %d, Steering: %d\n", throttle_, steering_);
    }

    // Wait for that esc
    key = cvWaitKey(10);
    if (char(key) == 27){
        break;
    }
  }

  // We're done
  cvReleaseCapture(&capture);
  cvDestroyWindow("Camera_Output");
  return 0;
}
