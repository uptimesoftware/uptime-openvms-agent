uptime-openvms-agent
====================

This is a technology preview of an upcoming up.time agent for monitoring openvms systems.Which allows you to capture the usual OS performance metrics for this things like:  

 - CPU Usage (Both total usage, and a multi-cpu breakdown )
 - Memory Utilization
 - Network Usage
 - File System Capacity & Disk Utilization 
 - etc

Requirements
============

 - This agent was written and tested against VMS 8.4 on Itanium Blades. 
 - Relies on the $getrmi service (So it should work as far back as VMS 7.3)
 - Probably needs at least GROUP, WORLD, and maybe PHY_IO privs. 


Agent Installation
==================

 As mentioned above, this is only a technology preview of an upcoming openvms agent for up.time. As such this version requires some additional installation & configuration steps that will not be present in the final agent.

 - Place the uptime-agent.c & uptimestat.c files on your openvms system.
 - Edit uptime-agent.c and modify the CPU constants on lines 188 - 190
 - Define your cc command, compile, link and run:
     - mycc :== “cc/warn=disable=(misalgndstrct)/lis “
     - mycc uptime-agent
     - link uptime-agent/sysexe
     - run uptime-agent
 - The agent will then start-up and output some initial debug infomation. Once the program goes into a 'waiting' mode, the agent is ready and can be discovered in up.time via either the 'Auto Discovery Wizard' or 'The add sytem/network '. Though the agent will identify itself as 'up.time agent 6.0.0  linux'.


Additional Notes
================

 - This version does not delete it's temporary files, so you may want to set the files nodname_PSINFO.TXT; (also nodename_DBUSY.TXT and nodename_WHOIN.TXT) to have a single file version.  (“$ set file/version=1 filename).
 - These temporary files won't exist untill after the first data collection.
 