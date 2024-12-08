ARPANET Network Control Program for open systems,
interfacing with the SIMH IMP emulator.

This code was originally written by Lars Brinkhoff, who originally wrote it 
and seemingly stopped working on it in late 2021. I have taken it upon myself
to update this, and get it into a usable state.

**This is a work in progress.**  At this point, the NCP is not a high
quality implementation.  It's primarily used for testing the network.

Lars's todo list:
- [x] Implement the IMP-host interface.
- [x] Exchange 1822 NOP messages between host NCP and IMP.
- [x] Send ECO message to another host, get ERP back.
- [x] Application library for NCP.
- [x] Send RFC and CLS messages to open and close connections.
- [x] Send data back and forth.
- [ ] ???
- [ ] Profit!

My todo list:
- [ ] Implement an inetd-style "pipe daemon" that allows for easy porting of
various programs.
- [ ] Implement a proxy utility that can translate between UDP and NCP sockets.
- [ ] Bake it into the Linux kernel.

### Building the NCP
```
cd src
make
```

### Using the NCP program
To communicate with clients, the NCP program uses a UNIX domain socket that
is stored in the environment variable `NCP`. In addition, the NCP also requires
the address of the remote system to talk to, the port on the remote system,
and the local port. These are all UDP ports, and can be whatever you want. Run
this in shell 1:
```
cd src
export NCP=/tmp/ncpsock1
./ncp localhost 22001 22002
```
Run this in shell 2:
```
cd src
export NCP=/tmp/ncpsock2
./ncp localhost 22002 22001
```
Now, in another shell, attempt a ping:
```
export NCP=/tmp/ncpsock2
./ping 1
```
It doesn't matter what address you enter for the remote host; any ping will
go through. 


### Building an NCP network
To do this, you must use IMPs. Included in the distribution is the IMP code,
as well as example scripts that start it up. This **requires** that you have
a recent version of SIMH compiled, and it must have the Honeywell 316
emulator present. In case you were unaware, the IMPs (Interface Message
Processors) on the ARPANET were H316s. 

Included in the `tests` directory is a basic point-to-point IMP network,
with the `impdemo1.simh` and `impdemo2.simh` files containing the SIMH 
control statements to bring up the network. `impdemo1` creates an IMP with 
address 2, and `impdemo2` creates an IMP with address 3. The "host interface"
ties into the "host," which, for us, is our NCP program we ran in the earlier
section. Go ahead and bring up the first IMP:
```
cd tests
simh-h316 impdemo1.simh
```
Bring up the second IMP:
```
cd tests
simh-h316 impdemo2.simh
```
Bring up the first NCP:
```
cd src
export NCP=/tmp/ncpsock1
./ncp localhost 22001 22002
```
Bring up the second NCP:
```
cd src
export NCP=/tmp/ncpsock1
./ncp localhost 22003 22004
```
You should now be able to ping host 3 from host 2. If that doesn't work, the
simulator is malfunctioning.
