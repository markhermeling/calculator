#!/bin/bash
kicad-cli sch export netlist -o model.cir --format spice Transistor_Calculator_V2.kicad_sch
sed -i "s/^.end//" model.cir
cat runTest.cmd >> model.cir
ngspice model.cir > results.output
python3 parseResults.py results.output