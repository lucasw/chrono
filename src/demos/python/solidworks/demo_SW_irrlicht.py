# =============================================================================
# PROJECT CHRONO - http://projectchrono.org
#
# Copyright (c) 2014 projectchrono.org
# All rights reserved.
#
# Use of this source code is governed by a BSD-style license that can be found
# in the LICENSE file at the top level of the distribution and at
# http://projectchrono.org/license-chrono.txt.
#
# =============================================================================


import pychrono as chrono
import pychrono.irrlicht as chronoirr

# The path to the Chrono data directory containing various assets (meshes, textures, data files)
# is automatically set, relative to the default location of this demo.
# If running from a different directory, you must change the path to the data directory with: 
#chrono.SetChronoDataPath('path/to/data')


# ---------------------------------------------------------------------
#
#  Create the simulation system.
#  (Do not create parts and constraints programmatically here, we will
#  load a mechanism from file)

sys = chrono.ChSystemNSC()


# Set the collision margins. This is expecially important for very large or
# very small objects (as in this example)! Do this before creating shapes.
chrono.ChCollisionModel.SetDefaultSuggestedEnvelope(0.001);
chrono.ChCollisionModel.SetDefaultSuggestedMargin(0.001);


# ---------------------------------------------------------------------
#
#  load the file generated by the SolidWorks CAD plugin
#  and add it to the system
#

print ("Loading C::E scene...");

exported_items = chrono.ImportSolidWorksSystem(chrono.GetChronoDataFile('solid_works/swiss_escapement'))

print ("...done!");

# Print exported items
for item in exported_items:
    print (item.GetName())

# Add items to the physical system
for item in exported_items:
    sys.Add(item)


# ---------------------------------------------------------------------
#
#  Create an Irrlicht application to visualize the system
#

vis = chronoirr.ChVisualSystemIrrlicht()
sys.SetVisualSystem(vis)
vis.SetWindowSize(1024,768)
vis.SetWindowTitle('Test: using data exported by Chrono::Solidworks')
vis.Initialize()
vis.AddLogo(chrono.GetChronoDataFile('logo_pychrono_alpha.png'))
vis.AddSkyBox()
vis.AddCamera(chrono.ChVectorD(0.3,0.3,0.4))
vis.AddTypicalLights()


# ---------------------------------------------------------------------
#
#  Run the simulation
#


sys.SetMaxPenetrationRecoverySpeed(0.002);


while vis.Run():
    vis.BeginScene()
    vis.DrawAll()
    vis.EndScene()
    sys.DoStepDynamics(0.002)





