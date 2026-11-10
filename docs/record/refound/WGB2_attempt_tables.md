# WGB2: the attempts of the wind-up gap bridge, 6 h and day pair

**STATUS**

- **id:** WGB2-T
- **topic:** network-vehicle
- **status:** reference
- **summary:** Companion table of [WGB2](WGB2_windup_bridge_current_golden_day_2026-10-31.md): every attempt of the bridge arms `_rap_wgbc_on_f6` (61 attempts) and `_rap_wgbc_on_f24` (280 attempts) with the bridge's whole-cycle count and the independent recount of `scripts/windup_gap_independent_check.py`.
- **source:** [WGB2](WGB2_windup_bridge_current_golden_day_2026-10-31.md) section 5; the tables `table_w6.csv` and `table_w24.csv` of `D:/GnssProcessorSupport/gates/wgbclose/` carry the further columns (K, J, origin, minimum |beta|, nominal yaw rate, largest step)
- **dates in the text:** 2026-10-31

An attempt is a gap of 300 s to under 900 s that the bridge was offered: the control's wind-up restarts in that range, TOW in GPS seconds of week 2313 (259200 is 00:00 UT of DOY 129), gap in seconds. **bridge**: the bridge arm's outcome (`bridged`, or the refusal named in its run-end line; `restart` is a refusal whose reason the first 100 run-end lines do not list). **bridge cycles**: the whole-cycle count of the run-end line, `-` when the attempt is a refusal or is beyond the first 100 lines of the run-end list. **independent**: the recount N = K + J (K carried into the gap, J gained inside it), K and J shown when N or J is not 0. **agreement**: `yes` equal counts, `yes (cut)` counts that differ by the flip of the cut (amendment A1), `NO` a disagreement, `not listed` a bridged attempt whose count the run-end lines do not give, `refused` an attempt the bridge did not bridge.

## W6, 61 attempts

| station | sat | TOW | gap s | bridge | bridge cycles | independent | agreement |
| --- | --- | --- | --- | --- | --- | --- | --- |
| SCTB00ATA | E18 | 260430 | 840 | bridged | 0 | 0 | yes |
| KIT300UZB | G02 | 262110 | 420 | bridged | 0 | 0 | yes |
| SCTB00ATA | E18 | 262230 | 480 | bridged | 0 | 0 | yes |
| MAC100AUS | G06 | 262350 | 330 | bridged | 0 | 0 | yes |
| BSHM00ISR | G20 | 262920 | 720 | umbra | - | 0 | refused |
| CEBR00ESP | G20 | 262920 | 720 | umbra | - | 0 | refused |
| CPVG00CPV | G20 | 262920 | 720 | umbra | - | 0 | refused |
| DJIG00DJI | G20 | 262920 | 720 | umbra | - | 0 | refused |
| GRAZ00AUT | G20 | 262920 | 720 | umbra | - | 0 | refused |
| MBAR00UGA | G20 | 262920 | 720 | umbra | - | 0 | refused |
| POVE00BRA | G20 | 262920 | 720 | umbra | - | 0 | refused |
| SAVO00BRA | G20 | 262920 | 720 | umbra | - | 0 | refused |
| SEYG00SYC | G20 | 262920 | 720 | umbra | - | 0 | refused |
| STHL00GBR | G20 | 262920 | 720 | umbra | - | 0 | refused |
| VOIM00MDG | G20 | 262920 | 720 | umbra | - | 0 | refused |
| WIND00NAM | G20 | 262920 | 720 | umbra | - | 0 | refused |
| YKRO00CIV | G20 | 262920 | 720 | umbra | - | 0 | refused |
| SCTB00ATA | E18 | 263130 | 660 | bridged | 0 | 0 | yes |
| SCTB00ATA | E18 | 264060 | 870 | bridged | 0 | 0 | yes |
| SCTB00ATA | E18 | 264960 | 330 | bridged | 0 | 0 | yes |
| SCTB00ATA | E18 | 265860 | 510 | bridged | 0 | 0 | yes |
| BSHM00ISR | G14 | 266550 | 300 | bridged | 0 | 0 | yes |
| SCTB00ATA | E18 | 266730 | 660 | bridged | 0 | 0 | yes |
| KIT300UZB | E33 | 267090 | 870 | bridged | 0 | 0 | yes |
| BOGT00COL | G32 | 267780 | 360 | bridged | 0 | 0 | yes |
| BOGT00COL | E08 | 268350 | 600 | bridged | 0 | 0 | yes |
| BOGT00COL | G32 | 268650 | 630 | bridged | 0 | 0 | yes |
| PARK00AUS | E33 | 269220 | 870 | bridged | 0 | 0 | yes |
| BOGT00COL | E08 | 269940 | 750 | bridged | 0 | 0 | yes |
| BSHM00ISR | G14 | 271050 | 720 | bridged | 0 | 0 | yes |
| BSHM00ISR | G14 | 271980 | 750 | bridged | 0 | 0 | yes |
| BOGT00COL | E30 | 272700 | 540 | bridged | 0 | 0 | yes |
| WARK00NZL | E14 | 273030 | 480 | bridged | 0 | 0 | yes |
| GCGO00USA | G22 | 273810 | 420 | bridged | 0 | 0 | yes |
| WARK00NZL | E14 | 273930 | 660 | bridged | 0 | 0 | yes |
| GOPE00CZE | E18 | 274830 | 840 | bridged | 0 | 0 | yes |
| WARK00NZL | E14 | 274830 | 840 | bridged | 0 | 0 | yes |
| DARW00AUS | G08 | 275250 | 420 | bridged | 0 | 0 | yes |
| DARW00AUS | G08 | 275730 | 390 | bridged | 0 | 0 | yes |
| GOPE00CZE | E18 | 275850 | 420 | bridged | 0 | 0 | yes |
| WARK00NZL | E14 | 276630 | 480 | bridged | 0 | 0 | yes |
| GOPE00CZE | E18 | 276660 | 510 | bridged | 0 | 0 | yes |
| SCTB00ATA | E14 | 276660 | 510 | bridged | 0 | 0 | yes |
| BOR100POL | E18 | 276750 | 600 | bridged | 0 | 0 | yes |
| SCTB00ATA | E14 | 277530 | 660 | bridged | 0 | 0 | yes |
| WARK00NZL | E14 | 277530 | 660 | bridged | 0 | 0 | yes |
| SGOC00LKA | G14 | 278100 | 570 | bridged | -1 | -1 (K -1, J 0) | yes |
| SCTB00ATA | E14 | 278460 | 870 | bridged | 0 | 0 | yes |
| WARK00NZL | E14 | 278460 | 870 | bridged | 0 | 0 | yes |
| BSHM00ISR | G19 | 278760 | 330 | bridged | 0 | 0 | yes |
| SCTB00ATA | E14 | 279330 | 300 | bridged | 0 | 0 | yes |
| GOPE00CZE | E18 | 279360 | 330 | bridged | 0 | 0 | yes |
| WARK00NZL | E14 | 279360 | 330 | bridged | 0 | 0 | yes |
| BOR100POL | E18 | 279480 | 450 | bridged | 0 | 0 | yes |
| KIT300UZB | G22 | 279690 | 480 | bridged | -1 | -1 (K -1, J 0) | yes |
| MAJU00MHL | G19 | 280020 | 330 | bridged | 0 | 0 | yes |
| SCTB00ATA | E14 | 280230 | 480 | bridged | 0 | 0 | yes |
| GOPE00CZE | E18 | 280260 | 510 | bridged | 0 | 0 | yes |
| WARK00NZL | E14 | 280260 | 510 | bridged | 0 | 0 | yes |
| BOR100POL | E18 | 280350 | 600 | bridged | 0 | 0 | yes |
| GAMB00PYF | G04 | 280530 | 360 | bridged | 0 | 0 | yes |

## W24, 280 attempts

| station | sat | TOW | gap s | bridge | bridge cycles | independent | agreement |
| --- | --- | --- | --- | --- | --- | --- | --- |
| SCTB00ATA | E18 | 260430 | 840 | bridged | - | 0 | not listed |
| KIT300UZB | G02 | 262110 | 420 | bridged | - | 0 | not listed |
| SCTB00ATA | E18 | 262230 | 480 | bridged | - | 0 | not listed |
| MAC100AUS | G06 | 262350 | 330 | bridged | 0 | 0 | yes |
| BSHM00ISR | G20 | 262920 | 720 | umbra | - | 0 | refused |
| CEBR00ESP | G20 | 262920 | 720 | umbra | - | 0 | refused |
| CPVG00CPV | G20 | 262920 | 720 | restart | - | 0 | refused |
| DJIG00DJI | G20 | 262920 | 720 | restart | - | 0 | refused |
| GRAZ00AUT | G20 | 262920 | 720 | restart | - | 0 | refused |
| MBAR00UGA | G20 | 262920 | 720 | umbra | - | 0 | refused |
| POVE00BRA | G20 | 262920 | 720 | umbra | - | 0 | refused |
| SAVO00BRA | G20 | 262920 | 720 | umbra | - | 0 | refused |
| SEYG00SYC | G20 | 262920 | 720 | umbra | - | 0 | refused |
| STHL00GBR | G20 | 262920 | 720 | umbra | - | 0 | refused |
| VOIM00MDG | G20 | 262920 | 720 | restart | - | 0 | refused |
| WIND00NAM | G20 | 262920 | 720 | umbra | - | 0 | refused |
| YKRO00CIV | G20 | 262920 | 720 | umbra | - | 0 | refused |
| SCTB00ATA | E18 | 263130 | 660 | bridged | - | 0 | not listed |
| SCTB00ATA | E18 | 264060 | 870 | bridged | - | 0 | not listed |
| SCTB00ATA | E18 | 264960 | 330 | bridged | - | 0 | not listed |
| SCTB00ATA | E18 | 265860 | 510 | bridged | - | 0 | not listed |
| BSHM00ISR | G14 | 266550 | 300 | bridged | 0 | 0 | yes |
| SCTB00ATA | E18 | 266730 | 660 | bridged | - | 0 | not listed |
| KIT300UZB | E33 | 267090 | 870 | bridged | - | 0 | not listed |
| BOGT00COL | G32 | 267780 | 360 | bridged | - | 0 | not listed |
| BOGT00COL | E08 | 268350 | 600 | bridged | - | 0 | not listed |
| BOGT00COL | G32 | 268650 | 630 | bridged | - | 0 | not listed |
| PARK00AUS | E33 | 269220 | 870 | bridged | 0 | 0 | yes |
| BOGT00COL | E08 | 269940 | 750 | bridged | - | 0 | not listed |
| BSHM00ISR | G14 | 271050 | 720 | bridged | 0 | 0 | yes |
| BSHM00ISR | G14 | 271980 | 750 | bridged | 0 | 0 | yes |
| BOGT00COL | E30 | 272700 | 540 | bridged | - | 0 | not listed |
| WARK00NZL | E14 | 273030 | 480 | bridged | 0 | 0 | yes |
| GCGO00USA | G22 | 273810 | 420 | bridged | 0 | 0 | yes |
| WARK00NZL | E14 | 273930 | 660 | bridged | 0 | 0 | yes |
| GOPE00CZE | E18 | 274830 | 840 | bridged | - | 0 | not listed |
| WARK00NZL | E14 | 274830 | 840 | bridged | 0 | 0 | yes |
| DARW00AUS | G08 | 275250 | 420 | bridged | - | 0 | not listed |
| DARW00AUS | G08 | 275730 | 390 | bridged | - | 0 | not listed |
| GOPE00CZE | E18 | 275850 | 420 | bridged | - | 0 | not listed |
| WARK00NZL | E14 | 276630 | 480 | bridged | 0 | 0 | yes |
| GOPE00CZE | E18 | 276660 | 510 | bridged | - | 0 | not listed |
| SCTB00ATA | E14 | 276660 | 510 | bridged | - | 0 | not listed |
| BOR100POL | E18 | 276750 | 600 | bridged | - | 0 | not listed |
| SCTB00ATA | E14 | 277530 | 660 | bridged | - | 0 | not listed |
| WARK00NZL | E14 | 277530 | 660 | bridged | 0 | 0 | yes |
| SGOC00LKA | G14 | 278100 | 570 | bridged | - | -1 (K -1, J 0) | not listed |
| SCTB00ATA | E14 | 278460 | 870 | bridged | - | 0 | not listed |
| WARK00NZL | E14 | 278460 | 870 | bridged | 0 | 0 | yes |
| BSHM00ISR | G19 | 278760 | 330 | bridged | 0 | 0 | yes |
| SCTB00ATA | E14 | 279330 | 300 | bridged | - | 0 | not listed |
| GOPE00CZE | E18 | 279360 | 330 | bridged | - | 0 | not listed |
| WARK00NZL | E14 | 279360 | 330 | bridged | 0 | 0 | yes |
| BOR100POL | E18 | 279480 | 450 | bridged | - | 0 | not listed |
| KIT300UZB | G22 | 279690 | 480 | bridged | - | -1 (K -1, J 0) | not listed |
| MAJU00MHL | G19 | 280020 | 330 | bridged | - | 0 | not listed |
| SCTB00ATA | E14 | 280230 | 480 | bridged | - | 0 | not listed |
| GOPE00CZE | E18 | 280260 | 510 | bridged | - | 0 | not listed |
| WARK00NZL | E14 | 280260 | 510 | bridged | 0 | 0 | yes |
| BOR100POL | E18 | 280350 | 600 | bridged | - | 0 | not listed |
| GAMB00PYF | G04 | 280530 | 360 | bridged | 0 | 0 | yes |
| KIT300UZB | G22 | 280800 | 630 | bridged | - | -1 (K -1, J 0) | not listed |
| SCTB00ATA | E14 | 281130 | 660 | bridged | - | 0 | not listed |
| WARK00NZL | E14 | 281160 | 690 | bridged | 0 | 0 | yes |
| GOPE00CZE | E18 | 281190 | 720 | bridged | - | 0 | not listed |
| SCTB00ATA | E14 | 282060 | 870 | bridged | - | 0 | not listed |
| GOPE00CZE | E18 | 282090 | 900 | bridged | - | 0 | not listed |
| WARK00NZL | E14 | 282090 | 900 | bridged | 0 | 0 | yes |
| BOGT00COL | E13 | 282180 | 360 | bridged | - | 0 | not listed |
| WROC00POL | E18 | 282600 | 870 | bridged | - | 0 | not listed |
| KIT300UZB | E31 | 282720 | 420 | bridged | - | -1 (K -1, J 0) | not listed |
| SCTB00ATA | E14 | 282960 | 330 | bridged | - | 0 | not listed |
| WARK00NZL | E14 | 282960 | 330 | bridged | 0 | 0 | yes |
| GOPE00CZE | E18 | 283020 | 390 | bridged | - | 0 | not listed |
| BOR100POL | E18 | 283050 | 420 | bridged | - | 0 | not listed |
| GOPE00CZE | E18 | 283860 | 510 | bridged | - | 0 | not listed |
| SCTB00ATA | E14 | 283860 | 510 | bridged | - | 0 | not listed |
| WARK00NZL | E14 | 283890 | 540 | bridged | - | 0 | not listed |
| BOR100POL | E18 | 283980 | 630 | bridged | - | 0 | not listed |
| GOPE00CZE | E18 | 284760 | 690 | bridged | - | 0 | not listed |
| SCTB00ATA | E14 | 284760 | 690 | bridged | - | 0 | not listed |
| WARK00NZL | E14 | 284790 | 720 | bridged | - | 0 | not listed |
| CEBR00ESP | E12 | 285210 | 420 | bridged | 0 | 0 | yes |
| GUAM00GUM | G11 | 285600 | 840 | bridged | 0 | 0 | yes |
| GOPE00CZE | E18 | 285660 | 870 | bridged | - | 0 | not listed |
| WARK00NZL | E14 | 285660 | 870 | bridged | - | 0 | not listed |
| SCTB00ATA | E14 | 285690 | 900 | bridged | - | 0 | not listed |
| SCTB00ATA | E14 | 286560 | 330 | bridged | - | 0 | not listed |
| GOPE00CZE | E18 | 286590 | 360 | bridged | - | 0 | not listed |
| WARK00NZL | E14 | 286710 | 480 | bridged | - | 0 | not listed |
| BOR100POL | E18 | 286800 | 570 | bridged | - | 0 | not listed |
| BOGT00COL | G03 | 287100 | 750 | bridged | - | 0 | not listed |
| DARW00AUS | G07 | 287100 | 360 | bridged | - | 0 | not listed |
| WROC00POL | E18 | 287400 | 840 | bridged | - | 0 | not listed |
| BOR100POL | E18 | 287490 | 540 | bridged | - | 0 | not listed |
| GOPE00CZE | E18 | 287490 | 540 | bridged | - | 0 | not listed |
| SCTB00ATA | E14 | 287490 | 540 | bridged | - | 0 | not listed |
| GOPE00CZE | E18 | 288360 | 690 | bridged | - | 0 | not listed |
| SCTB00ATA | E14 | 288360 | 690 | bridged | - | 0 | not listed |
| BOGT00COL | G28 | 288570 | 300 | bridged | - | 0 | not listed |
| SCTB00ATA | E14 | 289260 | 870 | bridged | - | 0 | not listed |
| BSHM00ISR | G20 | 290130 | 720 | bridged | 0 | 0 | yes |
| LMMF00MTQ | E14 | 290160 | 330 | bridged | 0 | 0 | yes |
| SCTB00ATA | E14 | 290220 | 390 | bridged | - | 0 | not listed |
| GOPE00CZE | E18 | 290250 | 420 | bridged | - | 0 | not listed |
| BOR100POL | E18 | 290280 | 450 | bridged | - | 0 | not listed |
| GOPE00CZE | E18 | 291060 | 510 | bridged | - | 0 | not listed |
| LMMF00MTQ | E14 | 291060 | 510 | bridged | 0 | 0 | yes |
| SCTB00ATA | E14 | 291120 | 570 | bridged | - | 0 | not listed |
| BOR100POL | E18 | 291180 | 630 | bridged | - | -1 (K 0, J -1) | not listed |
| LMMF00MTQ | E14 | 291960 | 690 | bridged | 0 | 0 | yes |
| BOR100POL | E18 | 291990 | 720 | bridged | - | -1 (K -1, J 0) | not listed |
| GOPE00CZE | E18 | 291990 | 720 | bridged | - | -1 (K -1, J 0) | not listed |
| WTZR00DEU | E18 | 292470 | 690 | bridged | - | 0 | not listed |
| JPLM00USA | G27 | 292680 | 600 | bridged | 0 | 0 | yes |
| GOPE00CZE | E18 | 292860 | 870 | bridged | - | -1 (K -1, J 0) | not listed |
| LMMF00MTQ | E14 | 292890 | 900 | bridged | 0 | 0 | yes |
| GOPE00CZE | E18 | 293760 | 330 | bridged | - | -1 (K -1, J 0) | not listed |
| LMMF00MTQ | E14 | 293760 | 330 | bridged | 0 | 0 | yes |
| BOR100POL | E18 | 293790 | 360 | bridged | - | 0 | not listed |
| KIT300UZB | G23 | 294450 | 390 | bridged | - | 0 | not listed |
| LMMF00MTQ | E14 | 294690 | 540 | bridged | 0 | 0 | yes |
| JPLM00USA | G08 | 294720 | 390 | bridged | 0 | 0 | yes |
| LMMF00MTQ | E14 | 295560 | 690 | bridged | 0 | 0 | yes |
| PARK00AUS | G15 | 295620 | 450 | bridged | 0 | 0 | yes |
| LMMF00MTQ | E14 | 296490 | 900 | bridged | 0 | 0 | yes |
| LMMF00MTQ | E14 | 297360 | 330 | bridged | 0 | 0 | yes |
| LMMF00MTQ | E14 | 298260 | 510 | bridged | 0 | 0 | yes |
| PARK00AUS | E09 | 298260 | 660 | bridged | 0 | 0 | yes |
| BOGT00COL | E21 | 298710 | 360 | bridged | - | 0 | not listed |
| LMMF00MTQ | E14 | 299160 | 690 | bridged | 0 | 0 | yes |
| LMMF00MTQ | E14 | 300960 | 330 | bridged | 0 | 0 | yes |
| WARK00NZL | E18 | 300960 | 330 | bridged | - | 0 | not listed |
| BSHM00ISR | G15 | 301170 | 810 | bridged | 0 | 0 | yes |
| LMMF00MTQ | E14 | 301860 | 510 | bridged | 0 | 0 | yes |
| SCTB00ATA | E18 | 301890 | 540 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 301920 | 570 | bridged | - | 0 | not listed |
| BSHM00ISR | G15 | 302280 | 510 | bridged | 0 | 0 | yes |
| LMMF00MTQ | E14 | 302760 | 690 | bridged | 0 | 0 | yes |
| WARK00NZL | E18 | 302760 | 690 | bridged | - | 0 | not listed |
| SCTB00ATA | E18 | 302790 | 720 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 303660 | 870 | bridged | - | 0 | not listed |
| LMMF00MTQ | E14 | 303690 | 900 | bridged | 0 | 0 | yes |
| SCTB00ATA | E18 | 303690 | 900 | bridged | - | 0 | not listed |
| MAJU00MHL | G15 | 303930 | 420 | bridged | - | 0 | not listed |
| CEBR00ESP | G01 | 304110 | 510 | bridged | -1 | -1 (K -1, J 0) | yes |
| BSHM00ISR | G10 | 304230 | 540 | bridged | 0 | 0 | yes |
| BSHM00ISR | G16 | 304230 | 540 | bridged | -1 | -1 (K -1, J 0) | yes |
| BSHM00ISR | G23 | 304230 | 540 | bridged | 0 | 0 | yes |
| BSHM00ISR | G27 | 304230 | 540 | bridged | 0 | 0 | yes |
| BSHM00ISR | G18 | 304260 | 570 | bridged | -1 | -1 (K -1, J 0) | yes |
| BSHM00ISR | G01 | 304350 | 660 | bridged | -1 | -1 (K -1, J 0) | yes |
| BSHM00ISR | G32 | 304350 | 660 | bridged | 0 | 0 | yes |
| BSHM00ISR | G26 | 304380 | 690 | bridged | 0 | 0 | yes |
| LMMF00MTQ | E14 | 304560 | 330 | bridged | 0 | 0 | yes |
| WARK00NZL | E18 | 304560 | 330 | bridged | - | 0 | not listed |
| SCTB00ATA | E18 | 304590 | 360 | bridged | - | 0 | not listed |
| SCTB00ATA | E18 | 305460 | 510 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 305460 | 510 | bridged | - | 0 | not listed |
| LMMF00MTQ | E14 | 305520 | 570 | bridged | 0 | 0 | yes |
| LMMF00MTQ | E14 | 306390 | 720 | bridged | 0 | 0 | yes |
| SCTB00ATA | E18 | 306390 | 720 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 306390 | 720 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 307260 | 870 | bridged | - | 0 | not listed |
| LMMF00MTQ | E14 | 307290 | 900 | bridged | 0 | 0 | yes |
| BOGT00COL | G19 | 307380 | 540 | bridged | - | 0 | not listed |
| LMMF00MTQ | E14 | 308160 | 330 | bridged | 0 | 0 | yes |
| WARK00NZL | E18 | 308160 | 330 | bridged | - | 1 (K 0, J 1) | not listed |
| SCTB00ATA | E18 | 308190 | 360 | bridged | - | 1 (K 0, J 1) | not listed |
| WARK00NZL | E18 | 309060 | 510 | bridged | - | 1 (K 1, J 0) | not listed |
| LMMF00MTQ | E14 | 309090 | 540 | bridged | 0 | 0 | yes |
| SCTB00ATA | E18 | 309090 | 540 | bridged | - | 1 (K 1, J 0) | not listed |
| LMMF00MTQ | E14 | 309990 | 720 | bridged | 0 | 0 | yes |
| SCTB00ATA | E18 | 309990 | 720 | bridged | - | 1 (K 1, J 0) | not listed |
| WARK00NZL | E18 | 309990 | 720 | bridged | - | 1 (K 1, J 0) | not listed |
| KIT300UZB | G18 | 310530 | 330 | bridged | - | -1 (K -1, J 0) | not listed |
| WARK00NZL | E18 | 310860 | 870 | bridged | - | 1 (K 1, J 0) | not listed |
| KIT300UZB | G18 | 311010 | 330 | bridged | - | -1 (K -1, J 0) | not listed |
| LMMF00MTQ | E14 | 311760 | 330 | bridged | 0 | 0 | yes |
| GOPE00CZE | E14 | 311790 | 360 | bridged | - | 0 | not listed |
| SCTB00ATA | E18 | 311790 | 360 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 311790 | 360 | bridged | - | 1 (K 1, J 0) | not listed |
| BOR100POL | E14 | 311820 | 390 | bridged | - | 0 | not listed |
| KIT300UZB | G18 | 312000 | 750 | bridged | - | -1 (K -1, J 0) | not listed |
| MAC100AUS | E02 | 312450 | 540 | bridged | 1 | 1 (K 0, J 1) | yes |
| GOPE00CZE | E14 | 312690 | 540 | bridged | - | 0 | not listed |
| LMMF00MTQ | E14 | 312690 | 540 | bridged | -1 | -1 (K 0, J -1) | yes |
| SCTB00ATA | E18 | 312690 | 540 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 312690 | 540 | bridged | - | 1 (K 1, J 0) | not listed |
| BOR100POL | E14 | 312750 | 600 | bridged | - | 0 | not listed |
| GOPE00CZE | E14 | 313590 | 720 | bridged | - | 0 | not listed |
| LMMF00MTQ | E14 | 313590 | 720 | bridged | -1 | -1 (K -1, J 0) | yes |
| SCTB00ATA | E18 | 313590 | 720 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 313590 | 720 | bridged | - | 1 (K 1, J 0) | not listed |
| PARK00AUS | G15 | 313950 | 330 | bridged | 0 | 0 | yes |
| GOPE00CZE | E14 | 314490 | 900 | bridged | - | 0 | not listed |
| LMMF00MTQ | E14 | 314490 | 900 | bridged | -1 | -1 (K -1, J 0) | yes |
| SCTB00ATA | E18 | 314490 | 900 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 314490 | 900 | bridged | - | 1 (K 1, J 0) | not listed |
| BOR100POL | E14 | 315390 | 360 | bridged | - | 0 | not listed |
| GOPE00CZE | E14 | 315390 | 360 | bridged | - | 0 | not listed |
| LMMF00MTQ | E14 | 315390 | 360 | bridged | -1 | -1 (K -1, J 0) | yes |
| SCTB00ATA | E18 | 315390 | 360 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 315420 | 390 | bridged | - | 1 (K 1, J 0) | not listed |
| GOPE00CZE | E14 | 316260 | 510 | bridged | - | 0 | not listed |
| LMMF00MTQ | E14 | 316290 | 540 | bridged | -1 | -1 (K -1, J 0) | yes |
| SCTB00ATA | E18 | 316290 | 540 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 316290 | 540 | bridged | - | 1 (K 1, J 0) | not listed |
| BOR100POL | E14 | 316320 | 570 | bridged | - | 0 | not listed |
| WTZR00DEU | E14 | 316980 | 570 | bridged | - | 0 | not listed |
| BOR100POL | E14 | 317190 | 720 | bridged | - | 0 | not listed |
| GOPE00CZE | E14 | 317190 | 720 | bridged | - | 0 | not listed |
| LMMF00MTQ | E14 | 317190 | 720 | bridged | -1 | -1 (K -1, J 0) | yes |
| WARK00NZL | E18 | 317190 | 720 | bridged | - | 1 (K 1, J 0) | not listed |
| BSHM00ISR | G08 | 317760 | 450 | bridged | 0 | 0 | yes |
| GOPE00CZE | E14 | 318090 | 900 | bridged | - | 0 | not listed |
| LMMF00MTQ | E14 | 318090 | 900 | bridged | -1 | -1 (K -1, J 0) | yes |
| WARK00NZL | E18 | 318090 | 900 | bridged | - | 1 (K 1, J 0) | not listed |
| DARW00AUS | G15 | 318510 | 360 | bridged | - | 0 | not listed |
| WTZR00DEU | E14 | 318720 | 540 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 318990 | 360 | bridged | - | 1 (K 1, J 0) | not listed |
| BOR100POL | E14 | 319020 | 390 | bridged | - | 0 | not listed |
| GOPE00CZE | E14 | 319020 | 390 | bridged | - | 0 | not listed |
| SCTB00ATA | E18 | 319050 | 420 | bridged | - | 0 | not listed |
| BOGT00COL | G09 | 319110 | 540 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 319890 | 540 | bridged | - | 1 (K 1, J 0) | not listed |
| SCTB00ATA | E18 | 319920 | 570 | bridged | - | 0 | not listed |
| GOPE00CZE | E14 | 319950 | 600 | bridged | - | 0 | not listed |
| WTZR00DEU | E14 | 320670 | 420 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 320790 | 720 | bridged | - | 1 (K 1, J 0) | not listed |
| GCGO00USA | E03 | 320820 | 450 | bridged | 0 | 0 | yes |
| GCGO00USA | E05 | 320820 | 450 | bridged | 0 | 0 | yes |
| GCGO00USA | E09 | 320820 | 450 | bridged | 0 | 0 | yes |
| GCGO00USA | E13 | 320820 | 450 | bridged | 0 | 0 | yes |
| GCGO00USA | E24 | 320820 | 450 | bridged | 0 | 0 | yes |
| GCGO00USA | E26 | 320820 | 450 | bridged | 0 | 0 | yes |
| GCGO00USA | E31 | 320820 | 450 | bridged | 0 | 0 | yes |
| GCGO00USA | E33 | 320820 | 450 | bridged | 0 | 0 | yes |
| GCGO00USA | G03 | 320820 | 450 | bridged | 0 | 0 | yes |
| GCGO00USA | G06 | 320820 | 450 | bridged | 0 | 0 | yes |
| GCGO00USA | G12 | 320820 | 450 | bridged | 0 | 0 | yes |
| GCGO00USA | G17 | 320820 | 450 | bridged | 0 | 0 | yes |
| GCGO00USA | G19 | 320820 | 450 | bridged | 0 | 0 | yes |
| GCGO00USA | G24 | 320820 | 450 | bridged | 0 | 0 | yes |
| GCGO00USA | G25 | 320820 | 450 | bridged | 0 | 0 | yes |
| GCGO00USA | G32 | 320820 | 450 | bridged | 0 | 0 | yes |
| KIT300UZB | G10 | 321360 | 330 | bridged | - | 0 | not listed |
| GCGO00USA | G28 | 322200 | 360 | bridged | 0 | 0 | yes |
| KIT300UZB | G10 | 322290 | 480 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 322590 | 360 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 323490 | 540 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 324390 | 720 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 325290 | 900 | bridged | - | 0 | not listed |
| KIT300UZB | E02 | 325950 | 390 | bridged | - | 0 | not listed |
| SCTB00ATA | E14 | 326190 | 360 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 326190 | 360 | bridged | - | 0 | not listed |
| SCTB00ATA | E14 | 327090 | 540 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 327090 | 540 | bridged | - | 0 | not listed |
| WARK00NZL | E18 | 327990 | 720 | bridged | - | 0 | not listed |
| SCTB00ATA | E14 | 329820 | 390 | bridged | - | 0 | not listed |
| DARW00AUS | G18 | 330660 | 360 | bridged | - | 1 (K 1, J 0) | not listed |
| SCTB00ATA | E14 | 330780 | 630 | bridged | - | 0 | not listed |
| DARW00AUS | G18 | 331470 | 540 | bridged | - | 1 (K 1, J 0) | not listed |
| SCTB00ATA | E14 | 331590 | 720 | bridged | - | 0 | not listed |
| BOGT00COL | G06 | 332640 | 600 | bridged | - | -1 (K -1, J 0) | not listed |
| MAJU00MHL | G26 | 332820 | 330 | bridged | - | 0 | not listed |
| BOGT00COL | G25 | 333330 | 780 | bridged | - | 0 | not listed |
| SCTB00ATA | E14 | 333420 | 390 | bridged | - | 0 | not listed |
| ULAB00MNG | G03 | 333840 | 300 | bridged | - | 0 | not listed |
| SCTB00ATA | E14 | 334350 | 600 | bridged | - | 0 | not listed |
| SCTB00ATA | E14 | 337020 | 390 | bridged | - | 0 | not listed |
| KIT300UZB | G08 | 337410 | 630 | bridged | - | 0 | not listed |
| MAJU00MHL | G16 | 338340 | 360 | bridged | - | 0 | not listed |
| KIT300UZB | G08 | 338700 | 540 | bridged | - | 0 | not listed |
| LMMF00MTQ | E18 | 340620 | 390 | bridged | 0 | 0 | yes |
| LMMF00MTQ | E18 | 341520 | 570 | bridged | 0 | 0 | yes |
| KIT300UZB | G04 | 342450 | 480 | bridged | - | 0 | not listed |
| WARK00NZL | E31 | 343410 | 300 | bridged | - | 0 | not listed |
| LMMF00MTQ | E18 | 344220 | 390 | bridged | 0 | 0 | yes |
| LMMF00MTQ | E18 | 345150 | 600 | bridged | 0 | 0 | yes |
