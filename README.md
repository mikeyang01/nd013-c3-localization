# Introduction
In this final project your goal will be to localize a car driving in simulation for at least 170m from the starting position and never exceeding a distance pose error of 1.2m.

The simulation car is equipped with a lidar, provided by the simulator at regular intervals are lidar scans. 

There is also a point cloud map map.pcd already available, and by using point registration matching between the map and scans localization for the car can be accomplished. 

This point cloud map has been extracted from the CARLA simulator.

# Requirements
When starting out the pose estimation stays set on the starting pose. 

Green box shows the pose estimate and red scan is now misaligned with the map. 

When a pose error of 1.2m is exceeded a message displays "TRY AGAIN"

# Project steps
Step 1: Filter scan using voxel filter

Step 2: Find pose transform by using ICP or NDT matching

Step 3: Transform the scan so it aligns with ego's actual pose and render that scan

# Result

