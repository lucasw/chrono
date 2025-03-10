// =============================================================================
// PROJECT CHRONO - http://projectchrono.org
//
// Copyright (c) 2014 projectchrono.org
// All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file at the top level of the distribution and at
// http://projectchrono.org/license-chrono.txt.
//
// =============================================================================
// Authors: Radu Serban
// =============================================================================
//
// Utility classes implementing PID steering controllers. The base class
// implements the basic functionality to control the error between the location
// of a sentinel point (a point at a look-ahead distance in front of the vehicle)
// and the current target point.
//
// Derived classes differ in how they specify the target point.  This can be the
// closest point to the sentinel point on a pre-defined curve path (currently
// using a ChBezierCurve) or from some other external sources (e.g. interfacing
// with a camera sensor).
//
// An object of this type can be used within a Chrono::Vehicle driver model to
// provide the steering output.
//
// =============================================================================

#include <cstdio>

#include "chrono/core/ChMathematics.h"

#include "chrono_vehicle/ChWorldFrame.h"
#include "chrono_vehicle/utils/ChSteeringController.h"
#include "chrono_vehicle/utils/ChUtilsJSON.h"

using namespace rapidjson;

namespace chrono {
namespace vehicle {

// -----------------------------------------------------------------------------
// Implementation of the base class ChSteeringController
// -----------------------------------------------------------------------------
ChSteeringController::ChSteeringController()
    : m_dist(0),
      m_sentinel(0, 0, 0),
      m_target(0, 0, 0),
      m_err(0),
      m_erri(0),
      m_errd(0),
      m_csv(nullptr),
      m_collect(false) {
    // Default PID controller gains all zero (no control).
    SetGains(0, 0, 0);
}

ChSteeringController::ChSteeringController(const std::string& filename)
    : m_sentinel(0, 0, 0), m_target(0, 0, 0), m_err(0), m_erri(0), m_errd(0), m_csv(nullptr), m_collect(false) {
    Document d;
    ReadFileJSON(filename, d);
    if (d.IsNull())
        return;

    m_Kp = d["Gains"]["Kp"].GetDouble();
    m_Ki = d["Gains"]["Ki"].GetDouble();
    m_Kd = d["Gains"]["Kd"].GetDouble();

    m_dist = d["Lookahead Distance"].GetDouble();

    GetLog() << "Loaded JSON: " << filename.c_str() << "\n";
}

ChSteeringController::~ChSteeringController() {
    delete m_csv;
}

void ChSteeringController::Reset(const ChVehicle& vehicle) {
    // Base class only calculates an updated sentinel location.
    m_sentinel =
        vehicle.GetChassisBody()->GetFrame_REF_to_abs().TransformPointLocalToParent(m_dist * ChWorldFrame::Forward());
    m_err = 0;
    m_erri = 0;
    m_errd = 0;
}

double ChSteeringController::Advance(const ChVehicle& vehicle, double step) {
    // Calculate current "sentinel" location.  This is a point at the look-ahead
    // distance in front of the vehicle.
    m_sentinel =
        vehicle.GetChassisBody()->GetFrame_REF_to_abs().TransformPointLocalToParent(m_dist * ChWorldFrame::Forward());

    // Calculate current "target" location.
    CalcTargetLocation();

    // If data collection is enabled, append current target and sentinel locations.
    if (m_collect) {
        *m_csv << vehicle.GetChTime() << m_target << m_sentinel << std::endl;
    }

    // The "error" vector is the projection onto the horizontal plane of the vector between sentinel and target.
    ChVector<> err_vec = m_target - m_sentinel;
    ChWorldFrame::Project(err_vec);

    // Calculate the sign of the angle between the projections of the sentinel
    // vector and the target vector (with origin at vehicle location).
    ChVector<> sentinel_vec = m_sentinel - vehicle.GetPos();
    ChWorldFrame::Project(sentinel_vec);
    ChVector<> target_vec = m_target - vehicle.GetPos();
    ChWorldFrame::Project(target_vec);

    double temp = Vdot(Vcross(sentinel_vec, target_vec), ChWorldFrame::Vertical());

    // Calculate current error (magnitude).
    double err = ChSignum(temp) * err_vec.Length();

    // Estimate error derivative (backward FD approximation).
    m_errd = (err - m_err) / step;

    // Calculate current error integral (trapezoidal rule).
    m_erri += (err + m_err) * step / 2;

    // Cache new error
    m_err = err;

    // Return PID output (steering value)
    return m_Kp * m_err + m_Ki * m_erri + m_Kd * m_errd;
}

void ChSteeringController::StartDataCollection() {
    // Return now if currently collecting data.
    if (m_collect)
        return;
    // Create the CSV_writer object if needed (first call to this function).
    if (!m_csv) {
        m_csv = new utils::CSV_writer("\t");
        m_csv->stream().setf(std::ios::scientific | std::ios::showpos);
        m_csv->stream().precision(6);
    }
    // Enable data collection.
    m_collect = true;
}

void ChSteeringController::StopDataCollection() {
    // Suspend data collection.
    m_collect = false;
}

void ChSteeringController::WriteOutputFile(const std::string& filename) {
    // Do nothing if data collection was never enabled.
    if (m_csv)
        m_csv->write_to_file(filename);
}

// -----------------------------------------------------------------------------
// Implementation of the derived class ChPathSteeringController.
// -----------------------------------------------------------------------------
ChPathSteeringController::ChPathSteeringController(std::shared_ptr<ChBezierCurve> path, bool isClosedPath)
    : m_path(path) {
    // Create a tracker object associated with the given path.
    m_tracker = std::unique_ptr<ChBezierCurveTracker>(new ChBezierCurveTracker(path, isClosedPath));
}

ChPathSteeringController::ChPathSteeringController(const std::string& filename,
                                                   std::shared_ptr<ChBezierCurve> path,
                                                   bool isClosedPath)
    : ChSteeringController(filename), m_path(path) {
    // Create a tracker object associated with the given path.
    m_tracker = std::unique_ptr<ChBezierCurveTracker>(new ChBezierCurveTracker(path, isClosedPath));
}

void ChPathSteeringController::CalcTargetLocation() {
    // Let the underlying tracker do its magic.
    m_tracker->calcClosestPoint(m_sentinel, m_target);
}

void ChPathSteeringController::Reset(const ChVehicle& vehicle) {
    // Let the base class calculate the current location of the sentinel point.
    ChSteeringController::Reset(vehicle);

    // Reset the path tracker with the new sentinel location.
    m_tracker->reset(m_sentinel);
}

// -----------------------------------------------------------------------------
// Implementation of the derived class ChPathSteeringControllerXT.
// This controller considers two or three input channels
//  - Lateral deviation (PDT1 controller)
//  - Heading error   (PT1 filter)
//  - Ackermann angle (PT1 filter) if used for a wheeled vehicle
// The filter gain parameter and time constants are canonical.
// The user can take influence on the controller by modifying the weighting
// factors for the input channels, which default to 1
// -----------------------------------------------------------------------------

ChPathSteeringControllerXT::ChPathSteeringControllerXT(std::shared_ptr<ChBezierCurve> path,
                                                       bool isClosedPath,
                                                       double max_wheel_turn_angle)
    : m_path(path),
      m_R_threshold(100000.0),
      m_max_wheel_turn_angle(25.0 * CH_C_DEG_TO_RAD),
      m_filters_initialized(false),
      m_T1_delay(30.0 / 1000.0),
      m_Kp(0.4),
      m_Wy(1),
      m_Wh(1),
      m_Wa(1),
      m_res(0) {
    // Create a tracker object associated with the given path.
    m_tracker = std::unique_ptr<ChBezierCurveTracker>(new ChBezierCurveTracker(path, isClosedPath));
    if (max_wheel_turn_angle > 0.0) {
        m_max_wheel_turn_angle = max_wheel_turn_angle;
    }
}

ChPathSteeringControllerXT::ChPathSteeringControllerXT(const std::string& filename,
                                                       std::shared_ptr<ChBezierCurve> path,
                                                       bool isClosedPath,
                                                       double max_wheel_turn_angle)
    : m_path(path),
      m_R_threshold(100000.0),
      m_max_wheel_turn_angle(25.0 * CH_C_DEG_TO_RAD),
      m_filters_initialized(false),
      m_T1_delay(30.0 / 1000.0),
      m_Kp(0.4),
      m_Wy(1),
      m_Wh(1),
      m_Wa(1),
      m_res(0) {
    // Create a tracker object associated with the given path.
    m_tracker = std::unique_ptr<ChBezierCurveTracker>(new ChBezierCurveTracker(path, isClosedPath));
    if (max_wheel_turn_angle > 0.0) {
        m_max_wheel_turn_angle = max_wheel_turn_angle;
    }

    Document d;
    ReadFileJSON(filename, d);
    if (d.IsNull())
        return;

    m_Kp = d["Gains"]["Kp"].GetDouble();
    m_Wy = d["Gains"]["Wy"].GetDouble();
    m_Wh = d["Gains"]["Wh"].GetDouble();
    m_Wa = d["Gains"]["Wa"].GetDouble();

    m_dist = d["Lookahead Distance"].GetDouble();

    GetLog() << "Loaded JSON: " << filename.c_str() << "\n";
}

void ChPathSteeringControllerXT::CalcTargetLocation() {
    // Let the underlying tracker do its magic.
    // we need more information about the path properties here:
    ChFrame<> tnb;

    m_tracker->calcClosestPoint(m_sentinel, tnb, m_pcurvature);

    m_target = tnb.GetPos();

    m_ptangent = tnb.GetRot().GetXaxis();
    m_pnormal = tnb.GetRot().GetYaxis();
    m_pbinormal = tnb.GetRot().GetZaxis();
}

void ChPathSteeringControllerXT::Reset(const ChVehicle& vehicle) {
    // Let the base class calculate the current location of the sentinel point.
    ChSteeringController::Reset(vehicle);

    // Reset the path tracker with the new sentinel location.
    m_tracker->reset(m_sentinel);
}

double ChPathSteeringControllerXT::CalcHeadingError(ChVector<>& a, ChVector<>& b) {
    double ang = 0.0;

    // test for velocity > 0
    ChWorldFrame::Project(m_vel);
    m_vel.Normalize();
    double speed = m_vel.Length();

    if (speed < 1) {
        // vehicle is standing still, we take the chassis orientation
        ChWorldFrame::Project(a);
        ChWorldFrame::Project(b);
        a.Normalize();
        b.Normalize();
    } else {
        // vehicle is running, we take the {x,y} velocity vector
        a = m_vel;
        ChWorldFrame::Project(b);
        b.Normalize();
    }

    // it might happen to cruise against the path definition (end->begin instead of begin->end),
    // then the the tangent points backwards to driving direction
    // the distance |ab| is > 1 then
    ChVector<> ab = a - b;
    double ltest = ab.Length();

    ChVector<> vpc;
    if (ltest < 1) {
        vpc = Vcross(a, b);
    } else {
        vpc = Vcross(a, -b);
    }
    ang = std::asin(ChWorldFrame::Height(vpc));

    return ang;
}

int ChPathSteeringControllerXT::CalcCurvatureCode(ChVector<>& a, ChVector<>& b) {
    // a[] is a unit vector pointing to the left vehicle side
    // b[] is a unit vector pointing to the instantanous curve center
    ChWorldFrame::Project(a);
    ChWorldFrame::Project(b);
    a.Normalize();
    b.Normalize();

    // In a left turn the distance between the two points will be nearly zero
    // in a right turn the distance will be around 2, at least > 1
    ChVector<> ab = a - b;
    double ltest = ab.Length();

    // What is a straight line? We define a threshold radius R_threshold
    // if the actual curvature is greater than 1/R_treshold, we are in a curve
    // otherwise we take this point as part of a straight line
    // m_pcurvature is always >= 0
    if (m_pcurvature <= 1.0 / m_R_threshold) {
        return 0;  // -> straight line
    }
    if (ltest < 1.0) {
        return 1;  // left bending curve
    }
    return -1;  // right bending curve
}

double ChPathSteeringControllerXT::CalcAckermannAngle() {
    // what's this?
    // R = vehicle turn radius
    // L = effective wheelbase
    // alpha = turn angle of front wheel
    //
    // R = L/sin(alpha)
    // delta = L/R = L * sin(alpha) / L
    //
    // alpha scales linearly with the steering input
    return sin(m_res * m_max_wheel_turn_angle);
}

double ChPathSteeringControllerXT::Advance(const ChVehicle& vehicle, double step) {
    auto& chassis_frame = vehicle.GetChassisBody()->GetFrame_REF_to_abs();  // chassis ref-to-world frame (ISO frame)
    auto& chassis_rot = chassis_frame.GetRot();                             // chassis ref-to-world rotation (ISO frame)

    // Calculate current "sentinel" location.  This is a point at the look-ahead
    // distance in front of the vehicle.
    m_sentinel = chassis_frame.TransformPointLocalToParent(m_dist * ChWorldFrame::Forward());
    m_vel = vehicle.GetPointVelocity(ChVector<>(0, 0, 0));
    if (!m_filters_initialized) {
        // first time we know about step size
        m_HeadErrDelay.Config(step, m_T1_delay);
        m_AckermannAngleDelay.Config(step, m_T1_delay);
        m_PathErrCtl.Config(step, 0.3, 0.15, m_Kp);
        m_filters_initialized = true;
    }
    // Calculate current "target" location.
    CalcTargetLocation();

    // If data collection is enabled, append current target and sentinel locations.
    if (m_collect) {
        *m_csv << vehicle.GetChTime() << m_target << m_sentinel << std::endl;
    }

    // The "error" vector is the projection onto the horizontal planeof
    // the vector between sentinel and target.
    ChVector<> err_vec = m_target - m_sentinel;
    ChWorldFrame::Project(err_vec);

    // Calculate the sign of the angle between the projections of the sentinel
    // vector and the target vector (with origin at vehicle location).
    ChVector<> sentinel_vec = m_sentinel - vehicle.GetPos();
    ChWorldFrame::Project(sentinel_vec);
    ChVector<> target_vec = m_target - vehicle.GetPos();
    ChWorldFrame::Project(target_vec);

    double temp = Vdot(Vcross(sentinel_vec, target_vec), ChWorldFrame::Vertical());

    // Calculate current lateral error.
    double y_err = ChSignum(temp) * err_vec.Length();

    double y_err_out = m_PathErrCtl.Filter(y_err);

    // Calculate the heading error
    ChVector<> veh_head = chassis_rot.GetXaxis();  // vehicle forward direction (ISO frame)
    ChVector<> path_head = m_ptangent;

    double h_err = CalcHeadingError(veh_head, path_head);

    double h_err_out = m_HeadErrDelay.Filter(h_err);

    // Calculate the Ackermann angle
    double a_err = CalcAckermannAngle();

    double a_err_out = m_AckermannAngleDelay.Filter(a_err);

    // Calculate the resultant steering signal
    double res = m_Wy * y_err_out + m_Wh * h_err_out + m_Wa * a_err_out;

    // Additional processing is necessary: counter steer constraint
    // in left bending curves only left steering allowed
    // in right bending curves only right steering allowed
    // |res| is never allowed to grow above 1

    ChVector<> veh_left = chassis_rot.GetYaxis();  // vehicle left direction (ISO frame)
    ChVector<> path_left = m_pnormal;
    int crvcode = CalcCurvatureCode(veh_left, path_left);

    switch (crvcode) {
        case 1:
            m_res = ChClamp<>(res, 0.0, 1.0);
            break;
        case -1:
            m_res = ChClamp<>(res, -1.0, 0.0);
            break;
        default:
        case 0:
            m_res = ChClamp<>(res, -1.0, 1.0);
            break;
    }

    return m_res;
}

// -----------------------------------------------------------------------------
// Implementation of the derived class ChPathSteeringControllerSR.
// -----------------------------------------------------------------------------
ChPathSteeringControllerSR::ChPathSteeringControllerSR(std::shared_ptr<ChBezierCurve> path,
                                                       bool isClosedPath,
                                                       double max_wheel_turn_angle,
                                                       double axle_space)
    : m_path(path),
      m_isClosedPath(isClosedPath),
      m_Klat(0),
      m_Kug(0),
      m_Tp(0.5),
      m_L(axle_space),
      m_delta(0),
      m_delta_max(max_wheel_turn_angle),
      m_umin(2),
      m_idx_curr(0) {
    // retireve points
    CalcPathPoints();
    if (m_isClosedPath) {
        GetLog() << "Path is closed.\n";
    } else {
        GetLog() << "Path is open.\n";
    }
}

ChPathSteeringControllerSR::ChPathSteeringControllerSR(const std::string& filename,
                                                       std::shared_ptr<ChBezierCurve> path,
                                                       bool isClosedPath,
                                                       double max_wheel_turn_angle,
                                                       double axle_space)
    : m_path(path),
      m_isClosedPath(isClosedPath),
      m_L(axle_space),
      m_delta(0),
      m_delta_max(max_wheel_turn_angle),
      m_umin(1),
      m_idx_curr(0) {
    // retireve points
    CalcPathPoints();

    Document d;
    ReadFileJSON(filename, d);
    if (d.IsNull())
        return;

    m_Klat = d["Gains"]["Klat"].GetDouble();
    m_Kug = d["Gains"]["Kug"].GetDouble();

    m_Tp = d["Preview Time"].GetDouble();

    GetLog() << "Loaded JSON: " << filename.c_str() << "\n";
}

void ChPathSteeringControllerSR::CalcPathPoints() {
    size_t np = m_path->getNumPoints();
    S_l.resize(np);
    R_l.resize(np);
    R_lu.resize(np);

    if (m_isClosedPath) {
        for (size_t i = 0; i < np; i++) {
            S_l[i] = m_path->getPoint(i);
        }
        for (size_t i = 0; i < np - 1; i++) {
            R_l[i] = S_l[i + 1] - S_l[i];
            R_lu[i] = R_l[i];
            R_lu[i].Normalize();
        }
        // connect the last point to the first point
        R_l[np - 1] = S_l[0] - S_l[np - 1];
        R_lu[np - 1] = R_l[np - 1];
        R_lu[np - 1].Normalize();
    } else {
        for (size_t i = 0; i < np; i++) {
            S_l[i] = m_path->getPoint(i);
        }
        for (size_t i = 0; i < np - 1; i++) {
            R_l[i] = S_l[i + 1] - S_l[i];
            R_lu[i] = R_l[i];
            R_lu[i].Normalize();
        }
        R_l[np - 1] = S_l[np - 1] - S_l[np - 2];
        R_lu[np - 1] = R_l[np - 1];
        R_lu[np - 1].Normalize();
        // push the last point forward for 100 meters, keeping the last direction
        // R_lu[np - 1] = R_lu[np - 2];
        // R_l[np - 1] = 100.0 * R_lu[np - 1];
        // S_l[np - 1] = S_l[np - 2] + R_l[np - 1];
        // push the first point backwards for 100 meters, keeping the first direction
        // R_l[0] = 100.0 * R_lu[0];
        // S_l[0] = S_l[1] - R_l[0];
    }
}

void ChPathSteeringControllerSR::Reset(const ChVehicle& vehicle) {
    // Let the base class calculate the current location of the sentinel point.
    ChSteeringController::Reset(vehicle);

    m_Klat = 0;
    m_Kug = 0;
}

void ChPathSteeringControllerSR::SetGains(double Klat, double Kug) {
    m_Klat = std::abs(Klat);
    m_Kug = ChClamp(Kug, 0.0, 5.0);
}

void ChPathSteeringControllerSR::SetPreviewTime(double Tp) {
    m_Tp = ChClamp(Tp, 0.2, 4.0);
}

double ChPathSteeringControllerSR::Advance(const ChVehicle& vehicle, double step) {
    const double g = 9.81;

    auto& chassis_frame = vehicle.GetChassisBody()->GetFrame_REF_to_abs();  // chassis ref-to-world frame
    auto& chassis_rot = chassis_frame.GetRot();                             // chassis ref-to-world rotation
    double u = vehicle.GetSpeed();                                          // vehicle speed

    // Calculate unit vector pointing to the yaw center
    ChVector<> n_g = chassis_rot.GetYaxis();  // vehicle left direction (ISO frame)
    ChWorldFrame::Project(n_g);               // projected onto horizontal plane (world frame)
    n_g.Normalize();                          // normalized

    // Calculate current "sentinel" location.
    // This is a point at the look-ahead distance in front of the vehicle.
    double R = 0;
    double ut = u > m_umin ? u : m_umin;
    double factor = ut * m_Tp;
    if (m_delta == 0.0) {
        m_sentinel = chassis_frame.TransformPointLocalToParent(factor * ChWorldFrame::Forward());
    } else {
        // m_Kug is in [°/g]
        R = (m_L + CH_C_DEG_TO_RAD * m_Kug * u * u / g) / m_delta;
        double theta = u * m_Tp / R;
        ChMatrix33<> RM(theta, ChWorldFrame::Vertical());
        m_sentinel = chassis_frame.TransformPointLocalToParent(factor * ChWorldFrame::Forward()) + R * (n_g - RM * n_g);
    }

    ChVector<> Pt = m_sentinel - S_l[m_idx_curr];
    double rt = R_l[m_idx_curr].Length();

    double t = std::abs(Pt.Dot(R_lu[m_idx_curr]));
    if (t >= rt) {
        while (t > rt) {
            m_idx_curr++;
            if (m_isClosedPath) {
                if (m_idx_curr == S_l.size()) {
                    m_idx_curr = 0;
                }
                Pt = m_sentinel - S_l[m_idx_curr];
                rt = R_l[m_idx_curr].Length();
                t = std::abs(Pt.Dot(R_lu[m_idx_curr]));
            } else {
                if (m_idx_curr == S_l.size()) {
                    m_idx_curr = S_l.size() - 1;
                }
                Pt = m_sentinel - S_l[m_idx_curr];
                rt = R_l[m_idx_curr].Length();
                t = std::abs(Pt.Dot(R_lu[m_idx_curr]));
                break;
            }
        }
    }

    m_target = S_l[m_idx_curr] + R_lu[m_idx_curr] * t;

    // If data collection is enabled, append current target and sentinel locations.
    if (m_collect) {
        *m_csv << vehicle.GetChTime() << m_target << m_sentinel << std::endl;
    }

    ChVector<> n_lu = R_lu[m_idx_curr] % ChWorldFrame::Vertical();  // cross product

    m_err = Pt.Dot(n_lu);

    if (u >= m_umin) {
        m_delta = ChClamp(m_delta + m_Klat * m_err, -m_delta_max, m_delta_max);
    }

    // Return steering value
    return m_delta / m_delta_max;
}

// -----------------------------------------------------------------------------
// Implementation of the derived class ChPathSteeringControllerStanley.
// -----------------------------------------------------------------------------
// This is called the "Stanley" Controller named after an autonomous vehicle called Stanley.
// It minimizes lateral error and heading error. Time delay of the driver's reaction is considered.
// This driver can be parametrized by a PID json file. It can consider a dead zone left and right to the
// path, where no path information is recognized. This can be useful when the path information contains
// lateral disturbances, that could cause bad disturbances of the controller.
// dead_zone = 0.05 means:
//     0 <= lat_err <= 0.05        no driver reaction
//     0.05 < lat_err <= 2*0.05    smooth transition interval to complete controller engagement
// The Stanley driver should find 'always' back to the path, despite of great heading or lateral error.
// If an integral term is used, its state is getting reset every 30 secs to avoid controller wind-up.
//
// The algorithm comes from from :
//
// Gabriel M. Hoffmann, Claire J. Tomlin, Michael Montemerlo, Sebastian Thrun:
// "Autonomous Automobile Trajectory Tracking for Off-Road Driving", 2005
// Stanford University
// Stanford, CA 94305, USA

ChPathSteeringControllerStanley::ChPathSteeringControllerStanley(std::shared_ptr<ChBezierCurve> path,
                                                                 bool isClosedPath,
                                                                 double max_wheel_turn_angle)
    : m_delayFilter(nullptr),
      m_path(path),
      m_isClosedPath(isClosedPath),
      m_delta(0),
      m_delta_max(max_wheel_turn_angle),
      m_umin(1),
      m_Treset(30.0),
      m_deadZone(0.0),
      m_Tdelay(0.4) {
    SetGains(0.0, 0.0, 0.0);
    m_tracker = std::unique_ptr<ChBezierCurveTracker>(new ChBezierCurveTracker(path, isClosedPath));
    if (m_isClosedPath) {
        GetLog() << "Path is closed.\n";
    } else {
        GetLog() << "Path is open.\n";
    }
}

ChPathSteeringControllerStanley::ChPathSteeringControllerStanley(const std::string& filename,
                                                                 std::shared_ptr<ChBezierCurve> path,
                                                                 bool isClosedPath,
                                                                 double max_wheel_turn_angle)
    : m_delayFilter(nullptr),
      m_path(path),
      m_isClosedPath(isClosedPath),
      m_delta(0),
      m_delta_max(max_wheel_turn_angle),
      m_umin(1),
      m_Treset(30.0),
      m_deadZone(0.0),
      m_Tdelay(0.4) {
    SetGains(0.0, 0.0, 0.0);
    m_tracker = std::unique_ptr<ChBezierCurveTracker>(new ChBezierCurveTracker(path, isClosedPath));
    Document d;
    ReadFileJSON(filename, d);
    if (d.IsNull())
        return;

    m_Kp = d["Gains"]["Kp"].GetDouble();
    m_Kd = d["Gains"]["Kd"].GetDouble();
    m_Ki = d["Gains"]["Ki"].GetDouble();

    if (d.HasMember("Lookahead Distance")) {
        m_dist = d["Lookahead Distance"].GetDouble();
    }
    if (d.HasMember("Dead Zone")) {
        m_deadZone = d["Dead Zone"].GetDouble();
    }
    GetLog() << "Loaded JSON: " << filename.c_str() << "\n";
}

void ChPathSteeringControllerStanley::Reset(const ChVehicle& vehicle) {
    // Let the base class calculate the current location of the sentinel point.
    ChSteeringController::Reset(vehicle);
}

void ChPathSteeringControllerStanley::SetGains(double Kp, double Ki, double Kd) {
    m_Kp = std::abs(Kp);
    m_Ki = std::abs(Ki);
    m_Kd = std::abs(Kd);
}

double ChPathSteeringControllerStanley::Advance(const ChVehicle& vehicle, double step) {
    if (m_delayFilter == nullptr) {
        m_delayFilter = std::shared_ptr<utils::ChFilterPT1>(new utils::ChFilterPT1(step, m_Tdelay));
    }
    auto& chassis_frame = vehicle.GetChassisBody()->GetFrame_REF_to_abs();  // chassis ref-to-world frame
    auto& chassis_rot = chassis_frame.GetRot();                             // chassis ref-to-world rotation
    double u = vehicle.GetSpeed();                                          // vehicle speed

    // Calculate current "sentinel" location.  This is a point at the look-ahead
    // distance in front of the vehicle.
    m_sentinel =
        vehicle.GetChassisBody()->GetFrame_REF_to_abs().TransformPointLocalToParent(m_dist * ChWorldFrame::Forward());

    // Calculate current "target" location.
    CalcTargetLocation();

    // If data collection is enabled, append current target and sentinel locations.
    if (m_collect) {
        *m_csv << vehicle.GetChTime() << m_target << m_sentinel << std::endl;
    }

    // The "error" vector is the projection onto the horizontal plane of the vector between sentinel and target.
    ChVector<> err_vec = m_target - m_sentinel;
    ChWorldFrame::Project(err_vec);

    // Calculate the sign of the angle between the projections of the sentinel
    // vector and the target vector (with origin at vehicle location).
    ChVector<> sentinel_vec = m_sentinel - vehicle.GetPos();
    ChWorldFrame::Project(sentinel_vec);
    ChVector<> target_vec = m_target - vehicle.GetPos();
    ChWorldFrame::Project(target_vec);

    double temp = Vdot(Vcross(sentinel_vec, target_vec), ChWorldFrame::Vertical());

    // Calculate current lateral error
    double err = ChSignum(temp) * err_vec.Length();
    double w = 1.0;
    if (m_deadZone > 0.0)
        w = ChSineStep(std::abs(err), m_deadZone, 0.0, 2.0 * m_deadZone, 1.0);
    err *= w;
    double err_dot = -u * sin(atan(m_Kp * err / ChClamp(u, m_umin, u)));
    // Calculate the heading error
    ChVector<> veh_head = chassis_rot.GetXaxis();  // vehicle forward direction (ISO frame)
    ChVector<> path_head = m_ptangent;

    // Calculate current error integral (trapezoidal rule).
    m_erri += (err + m_err) * step / 2;

    // Cache new error
    m_err = err;

    double h_err = CalcHeadingError(veh_head, path_head);

    // control law
    m_delta = h_err + atan(m_Kp * err / ChClamp(u, m_umin, u)) + m_Kd * err_dot + m_Ki * m_erri;
    double steer = ChClamp(m_delta / m_delta_max, -1.0, 1.0);
    m_Treset -= step;
    if (m_Treset <= 0.0) {
        m_Treset = 30.0;
        m_err = 0.0;
    }
    // Return steering value
    return m_delayFilter->Filter(steer);
}

void ChPathSteeringControllerStanley::CalcTargetLocation() {
    // Let the underlying tracker do its magic.
    // we need more information about the path properties here:
    ChFrame<> tnb;

    m_tracker->calcClosestPoint(m_sentinel, tnb, m_pcurvature);
    m_target = tnb.GetPos();
    m_ptangent = tnb.GetRot().GetXaxis();
}

double ChPathSteeringControllerStanley::CalcHeadingError(ChVector<>& a, ChVector<>& b) {
    double ang = 0.0;

    // chassis orientation
    ChWorldFrame::Project(a);
    ChWorldFrame::Project(b);
    a.Normalize();
    b.Normalize();

    ChVector<> vpc;
    vpc = Vcross(a, b);
    ang = std::asin(ChWorldFrame::Height(vpc));

    return ang;
}

}  // end namespace vehicle
}  // end namespace chrono
