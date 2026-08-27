// Snippet from SfzImporter.cpp opcode parsing loop:
if (opcode == "eq1_freq")      zone.eq1Freq = floatOpcode (val, 80.0f);
else if (opcode == "eq1_gain") zone.eq1Gain = floatOpcode (val, 0.0f);
else if (opcode == "eq1_bw")   zone.eq1Bw   = floatOpcode (val, 1.0f);
else if (opcode == "eq2_freq") zone.eq2Freq = floatOpcode (val, 1000.0f);
else if (opcode == "eq2_gain") zone.eq2Gain = floatOpcode (val, 0.0f);
else if (opcode == "eq2_bw")   zone.eq2Bw   = floatOpcode (val, 1.0f);
else if (opcode == "eq3_freq") zone.eq3Freq = floatOpcode (val, 8000.0f);
else if (opcode == "eq3_gain") zone.eq3Gain = floatOpcode (val, 0.0f);
else if (opcode == "eq3_bw")   zone.eq3Bw   = floatOpcode (val, 1.0f);