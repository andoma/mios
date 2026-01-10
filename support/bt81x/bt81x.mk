GLOBALDEPS += ${T}support/bt81x/bt81x.mk

${O}/support/bt81x_bitmapgen: ${T}support/bt81x/src/bt81x_bitmapgen.c ${GLOBALDEPS}
	mkdir -p "${O}/support"
	@echo "\tHOSTCC\t$<"
	$(CC) -O3 -o $@ $< -lm -lz

${O}/include/%_l4.h: %.png ${O}/support/bt81x_bitmapgen ${GLOBALDEPS}
	@echo "\tGEN\t$<"
	@mkdir -p $(dir $@)
	${O}/support/bt81x_bitmapgen $< l4 $@ $(notdir $(subst -,_,$*))

${O}/include/%_png.h: %.png ${O}/support/bt81x_bitmapgen ${GLOBALDEPS}
	@echo "\tGEN\t$<"
	@mkdir -p $(dir $@)
	${O}/support/bt81x_bitmapgen $< png $@ $(notdir $(subst -,_,$*))

${O}/include/%_jpeg.h: %.jpeg ${O}/support/bt81x_bitmapgen ${GLOBALDEPS}
	@echo "\tGEN\t$<"
	@mkdir -p $(dir $@)
	${O}/support/bt81x_bitmapgen $< jpeg $@ $(notdir $(subst -,_,$*))
