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
// Demo for universal joint.
//
// Recall that Irrlicht uses a left-hand frame, so everything is rendered with
// left and right flipped.
//
// =============================================================================

#include <cmath>

#include "chrono/physics/ChBody.h"
#include "chrono/physics/ChLinkMotorRotationAngle.h"
#include "chrono/physics/ChSystemNSC.h"
#include "chrono/core/ChRealtimeStep.h"

#include "chrono_irrlicht/ChVisualSystemIrrlicht.h"

using namespace chrono;
using namespace chrono::irrlicht;

using namespace irr;

// -----------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    GetLog() << "Copyright (c) 2017 projectchrono.org\nChrono version: " << CHRONO_VERSION << "\n\n";

    ChSystemNSC sys;

    // Disable gravity
    sys.Set_G_acc(ChVector<>(0, 0, 0));

    // Set the half-length of the two shafts
    double hl = 2;

    // Set the bend angle between the two shafts (positive rotation about the
    // global X axis)
    double angle = CH_C_PI / 6;
    double cosa = std::cos(angle);
    double sina = std::sin(angle);
    ChQuaternion<> rot = Q_from_AngX(angle);

    // Create the ground (fixed) body
    // ------------------------------

    auto ground = chrono_types::make_shared<ChBody>();
    sys.AddBody(ground);
    ground->SetIdentifier(-1);
    ground->SetBodyFixed(true);
    ground->SetCollide(false);

    // attach visualization assets to represent the revolute and cylindrical
    // joints that connect the two shafts to ground
    {
        auto cyl_1 = chrono_types::make_shared<ChCylinderShape>();
        cyl_1->GetCylinderGeometry().p1 = ChVector<>(0, 0, -hl - 0.2);
        cyl_1->GetCylinderGeometry().p2 = ChVector<>(0, 0, -hl + 0.2);
        cyl_1->GetCylinderGeometry().rad = 0.3;
        ground->AddVisualShape(cyl_1);

        auto cyl_2 = chrono_types::make_shared<ChCylinderShape>();
        cyl_2->GetCylinderGeometry().p1 = ChVector<>(0, -(hl - 0.2) * sina, (hl - 0.2) * cosa);
        cyl_2->GetCylinderGeometry().p2 = ChVector<>(0, -(hl + 0.2) * sina, (hl + 0.2) * cosa);
        cyl_2->GetCylinderGeometry().rad = 0.3;
        ground->AddVisualShape(cyl_2);
    }

    // Create the first shaft body
    // ---------------------------

    auto shaft_1 = chrono_types::make_shared<ChBody>();
    sys.AddBody(shaft_1);
    shaft_1->SetIdentifier(1);
    shaft_1->SetBodyFixed(false);
    shaft_1->SetCollide(false);
    shaft_1->SetMass(1);
    shaft_1->SetInertiaXX(ChVector<>(1, 1, 0.2));
    shaft_1->SetPos(ChVector<>(0, 0, -hl));
    shaft_1->SetRot(ChQuaternion<>(1, 0, 0, 0));

    // Add visualization assets to represent the shaft (a box) and the arm of the
    // universal joint's cross associated with this shaft (a cylinder)
    {
        auto box_1 = chrono_types::make_shared<ChBoxShape>();
        box_1->GetBoxGeometry().Size = ChVector<>(0.15, 0.15, 0.9 * hl);
        box_1->SetColor(ChColor(0.6f, 0, 0));
        shaft_1->AddVisualShape(box_1);

        auto cyl_2 = chrono_types::make_shared<ChCylinderShape>();
        cyl_2->GetCylinderGeometry().p1 = ChVector<>(-0.2, 0, hl);
        cyl_2->GetCylinderGeometry().p2 = ChVector<>(0.2, 0, hl);
        cyl_2->GetCylinderGeometry().rad = 0.05;
        cyl_2->SetColor(ChColor(0.6f, 0, 0));
        shaft_1->AddVisualShape(cyl_2);
    }

    // Create the second shaft body
    // ----------------------------

    // The second shaft is identical to the first one, but initialized at an angle
    // equal to the specified bend angle.

    auto shaft_2 = chrono_types::make_shared<ChBody>();
    sys.AddBody(shaft_2);
    shaft_2->SetIdentifier(1);
    shaft_2->SetBodyFixed(false);
    shaft_2->SetCollide(false);
    shaft_2->SetMass(1);
    shaft_2->SetInertiaXX(ChVector<>(1, 1, 0.2));
    shaft_2->SetPos(ChVector<>(0, -hl * sina, hl * cosa));
    shaft_2->SetRot(rot);

    // Add visualization assets to represent the shaft (a box) and the arm of the
    // universal joint's cross associated with this shaft (a cylinder)
    {
        auto box_1 = chrono_types::make_shared<ChBoxShape>();
        box_1->GetBoxGeometry().Size = ChVector<>(0.15, 0.15, 0.9 * hl);
        box_1->SetColor(ChColor(0, 0, 0.6f));
        shaft_2->AddVisualShape(box_1);

        auto cyl_2 = chrono_types::make_shared<ChCylinderShape>();
        cyl_2->GetCylinderGeometry().p1 = ChVector<>(0, -0.2, -hl);
        cyl_2->GetCylinderGeometry().p2 = ChVector<>(0, 0.2, -hl);
        cyl_2->GetCylinderGeometry().rad = 0.05;
        cyl_2->SetColor(ChColor(0, 0, 0.6f));
        shaft_2->AddVisualShape(cyl_2);
    }

    // Connect the first shaft to ground
    // ---------------------------------

    // Use a rotational motor to impose both the revolute joint constraints, as well
    // as constant angular velocity. Here, we drive the motor angle with a ramp function.
    // Alternatively, we could use a ChLinkMotorAngularSpeed with constant speed.
    // The joint is located at the origin of the first shaft.
    auto motor = chrono_types::make_shared<ChLinkMotorRotationAngle>();
    motor->Initialize(ground, shaft_1, ChFrame<>(ChVector<>(0, 0, -hl), ChQuaternion<>(1, 0, 0, 0)));
    motor->SetAngleFunction(chrono_types::make_shared<ChFunction_Ramp>(0, 1));
    sys.AddLink(motor);

    // Connect the second shaft to ground through a cylindrical joint
    // --------------------------------------------------------------

    // Use a cylindrical joint so that we do not have redundant constraints
    // (note that, technically Chrono could deal with a revolute joint here).
    // the joint is located at the origin of the second shaft.

    auto cyljoint = chrono_types::make_shared<ChLinkLockCylindrical>();
    sys.AddLink(cyljoint);
    cyljoint->Initialize(ground, shaft_2, ChCoordsys<>(ChVector<>(0, -hl * sina, hl * cosa), rot));

    // Connect the two shafts through a universal joint
    // ------------------------------------------------

    // The joint is located at the global origin.  Its kinematic constraints will
    // enforce orthogonality of the associated cross.

    auto ujoint = chrono_types::make_shared<ChLinkUniversal>();
    sys.AddLink(ujoint);
    ujoint->Initialize(shaft_1, shaft_2, ChFrame<>(ChVector<>(0, 0, 0), rot));

    // Create the Irrlicht application
    // -------------------------------

    // Create the Irrlicht visualization sys
    auto vis = chrono_types::make_shared<ChVisualSystemIrrlicht>();
    sys.SetVisualSystem(vis);
    vis->SetWindowSize(800, 600);
    vis->SetWindowTitle("Universal joint");
    vis->Initialize();
    vis->AddLogo();
    vis->AddSkyBox();
    vis->AddCamera(ChVector<>(3, 1, -1.5));
    vis->AddTypicalLights();

    // Simulation loop
    int frame = 0;
    double timestep = 0.005;
    ChRealtimeStepTimer realtime_timer;

    while (vis->Run()) {
        vis->BeginScene();
        vis->DrawAll();
        vis->EndScene();

        if (frame % 20 == 0) {
            // Output the shaft angular velocities at the current time
            double omega_1 = shaft_1->GetWvel_loc().z();
            double omega_2 = shaft_2->GetWvel_loc().z();
            GetLog() << sys.GetChTime() << "   " << omega_1 << "   " << omega_2 << "\n";
        }

        sys.DoStepDynamics(timestep);
        realtime_timer.Spin(timestep);
        frame++;
    }

    return 0;
}
