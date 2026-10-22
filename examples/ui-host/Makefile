# A host that is not nx-runtime: its own window and frames, the nx layers for menu, input, settings and saves.
# Proves the layers stand alone (nothing of nxrt/runtime is compiled or linked) and shows the wiring a runtime needs.
NXRT := ../..
include $(NXRT)/nx.mk

EXE := $(B)/uihost
all: $(EXE)
$(B)/host.o: host.mm $(NX_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(NXCXXFLAGS) $(INC_UI) $(NX_UIFLAGS) -c $< -o $@
$(EXE): $(B)/host.o $(NX_OBJ) $(NX_IMGUI_OBJ)
	$(CXX) -o $@ $^ $(NX_LDFLAGS)
.PHONY: all
