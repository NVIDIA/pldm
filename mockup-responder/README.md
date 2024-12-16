## Overview of PLDM Mockup Responder

A PLDM mockup responder is developed for developing pldmd on QEMU. The  PLDM 
mockup responder includes modified MCTP control and demux daemon, user can create
a emulated MCTP endpoint by providing a json file to modified MCTP control
daemon to expose the emulated MCTP Endpoint to D-Bus.
The PLDM mockup responder is a program listening to demux unix socket for
the request from pldmd/pldmtool and returning the respond to pldmd through
modified MCTP demux daemon.

Please refer the PLDM Mockup Responder design doc for more details.
https://docs.google.com/document/d/1TWOLCKsguaJi2vJWOjk8vdkX5xlG0eWP/edit?pli=1#heading=h.1ksv4uv

## Arguments details

Mockup will need below CLI arguments:

EID {-e}        Eid to be assigned to the mockup device
pdrFile {-p}    pdr.json file for the PDRs of system to be exposed by mockup

Please refer the help for more details.

## How to run

User can see the required arguments for PLDM Mockup with the **-h** help option as shown below:

```
root@hgxb:~# pldm_mockup_responder -h                                          
Usage: mockup_responder [options]
Options:
 [--verbose] - would enable verbosity
 [--eid <EID>] - assign EID to mockup responder
 [--pdrFile <Path>] - path to PDR file
root@hgxb:~# 
```
For running the PLDM Mockup user can run command as shown below.

```
root@hgxb:~# pldm_mockup_responder -e 31 -p /tmp/pdr.json -v
<6> start a Mockup Responder EID=31
<6> PDR file path=/tmp/pdr.json
<6> connect to Mockup EID(31)
<6> numericEffecterPDRs
<6> stateEffecterPDRs
...........
```
Please refer the detailed document on how to setup and run PLDM mockup Responder for more details
https://docs.google.com/document/d/1jrYW8PhmSFW6ZbZ-pYs91DhRTR10eK8HlpRtczKPiU0/edit?addon_store&tab=t.0
