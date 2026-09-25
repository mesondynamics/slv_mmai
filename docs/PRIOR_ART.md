# Prior art and related systems

These references provide technical and historical context for electronic
interfaces to existing mobile machinery. The relevance notes describe connections
to MMAI's architecture; they do not imply identical implementations.

## Electronic and electrohydraulic machine control

**Hydraulic control system for a street sweeper — US4343060A.**
Donald L. Hildebrand and Ernest F. Prescott. Priority/filing: 1980-07-18;
publication: 1982-08-10. Describes speed sensing and electronic control of a
hydraulic pump's displacement to regulate sweeping mechanisms. It is an early
example of electrical feedback and hydraulic actuation on mobile machinery.
[Patent publication](https://patents.google.com/patent/US4343060A/en).

**Remote control modification for manually controlled hydraulic systems —
CA2019542C.** Louis G. St. Martin. Priority: 1989-06-28; application publication
CA2019542A1: 1990-12-28; granted publication CA2019542C: 2000-06-27. Describes
adding remote electrical/electrohydraulic actuation to a manually controlled
hydraulic steering system, including a parallel control path.
[Patent publication](https://patents.google.com/patent/CA2019542C/en).

## Electronic vehicle-control architectures

**Extending Technology to Drive By Wire Control — SAE Technical Paper 941191.**
Patrick Donoghue, Gregory Lee Larson, and Stephan Dondoe. Published 1994-04-01
according to the SAE catalog. Describes full-authority electronic drive control
within an integrated ground-vehicle electronics architecture, including feedback
and vehicle-dynamics control. DOI: `10.4271/941191`.
[SAE publication](https://saemobilus.sae.org/papers/extending-technology-drive-wire-control-941191).

**Driver control input device for drive-by-wire vehicle — US20040099468A1.**
Adrian Chernoff, Joseph Szczerba, and Julien Montousse. Priority/filing: 2002-11-26;
publication: 2004-05-27. Describes electrical driver-input signals and their use
with vehicle control and electromechanical/electrohydraulic actuators.
[Patent publication](https://patents.google.com/patent/US20040099468A1/en).

## Retrofit and vehicle-interface examples

**AIce — MRSD Project Team I, Carnegie Mellon University.** Project year: 2022.
The team's full-system account describes the transition through an RC car and
ATV to a drive-by-wire Zamboni. Its Zamboni description includes steering-shaft
motor actuation, a CAN throttle interface, an electrohydraulic braking path, and
ROS communication with an ECU. This is a concrete example of integrating
upper-level autonomy with distinct actuation interfaces.
[Project account](https://mrsdprojects.ri.cmu.edu/2022teami/implementation/full-system/).

**Otonos Vehicle Interface Controller — Applied Research Associates.**
Product documentation; publication date not stated, accessed 2026-09-24.
Describes translation of ROS/JAUS commands into vehicle-specific control and
feedback, direct CAN integration, and an optional controller for other digital,
analog, or manual-control interfaces.
[Manufacturer description](https://www.ara.com/otonos/products/).

## Circuit-level reference

**CN0415 Circuit Note — Analog Devices.** Reference identifier: CN0415;
landing-page publication date not stated, accessed 2026-09-24. Describes
proportional/two-state solenoid drive, PWM, current sensing, closed-loop control,
dither, and overcurrent protection. It helps explain the electrical functions
in a valve driver. Its high-side sensing arrangement differs from the low-side
shunt placement in the MMAI ECU example.
[Manufacturer circuit note](https://www.analog.com/en/resources/reference-designs/circuits-from-the-lab/cn0415.html).

The [ECU design notes](../pcb/roller/ecu/README.zh-CN.md) retain additional
component and system references associated with the schematic pages. A reference
supports only the specific circuit function or architecture it actually describes.
