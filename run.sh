#!/bin/bash
kicad-cli sch export netlist -o model.cir --format spice Transistor_Calculator_V2.kicad_sch
ngspice model.cir