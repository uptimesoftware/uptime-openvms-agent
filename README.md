uptime-openvms-agent
====================

This is a technology preview of an upcoming up.time agent for monitoring openvms systems.Which allows you to capture the usual OS performance metrics for this things like:  

 - CPU Usage (Both total usage, and a multi-cpu breakdown )
 - Memory Utilization
 - Network Usage
 - File System Capacity & Disk Utilization 
 - etc


Agent Installation
==================

 As mentioned above, this is only a technology preview of an upcoming openvms agent for up.time. As such this version requires some additional installation & configuration steps that will not be present in the final agent.

 - Place the uptime-agent.c & uptimestat.c files on your openvms system.
 - Edit uptime-agent.c and modify the CPU constants on lines 188 - 190
 - Compile ONLY with /warn=disable=(misalgndstrct)
 - Link with /sysexe to pick up system data cells.



Requirements
============

 - This agent was written and tested against VMS 8.4 on Itanium Blades. 
 - Relies on the $getrmi service (So it should work as far back as VMS 7.3)
 - Probably needs at least GROUP, WORLD, and maybe PHY_IO privs. 



 

 