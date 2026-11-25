ManteTwin - Proyecto de deriva (escenario 1) en KiCad
=====================================================

nodo-deriva/   Placa del nodo: zocalos para el ESP32-DevKitC V4, dos conectores Grove
               para las placas SHT30 (principal 0x44 y referencia 0x45), LEDs de estado
               (verde GPIO25, rojo GPIO26, amarillo GPIO27), jumper FALLO en GPIO34
               con pull-down de 10k y puntos de test del bus I2C.
               Abrir: nodo-deriva/mantetwin-nodo-deriva.kicad_pro

placa-sht30/   Placa del sensor SHT30 (se fabrica dos veces):
               JP1 1-2 = 0x44 (principal), JP1 2-3 = 0x45 (referencia).
               Abrir: placa-sht30/mantetwin-sht30.kicad_pro

Mismos pines que el proyecto Wokwi 01-deriba: el firmware (sketch.ino) no cambia.

Antes de pasar al PCB:
 1. Inspect -> Electrical Rules Checker (ERC).
 2. Tools -> Assign Footprints: comprobar que KiCad encuentra todas las huellas.
 3. Medir con la placa real la separacion entre las dos filas del DevKitC (J2/J3)
    y colocar los zocalos a esa distancia en el PCB.
 4. Conector Grove: se ha usado la huella JST PH 2,0 mm; comprobarla con el conector comprado.
