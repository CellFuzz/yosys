"""Handwritten storage contracts from kernel/ff.cc and word_reg.rst.

This table deliberately does not use FfData or the production classifier.
It checks each published manifest independently, including normalized const D.
"""
import json


def check_register_semantics(run, work):
    # type, parameters, input pins, latch, controls, reset values, priority
    cases = [
        ("$sr", "SET_POLARITY=0 CLR_POLARITY=1", "SET=set CLR=clr", True,
         {"set": ("set", False), "clear": ("clr", True)}, None, None, False),
        ("$dlatch", "EN_POLARITY=0", "EN=en D=d", True,
         {"async_load": ("en", False)}, None, None, False),
        ("$adlatch", "EN_POLARITY=1 ARST_POLARITY=0 ARST_VALUE=3'101", "EN=en ARST=rst D=d", True,
         {"async_load": ("en", True), "async_reset": ("rst", False)}, "101", None, False),
        ("$dlatchsr", "EN_POLARITY=0 SET_POLARITY=1 CLR_POLARITY=0", "EN=en SET=set CLR=clr D=d", True,
         {"async_load": ("en", False), "set": ("set", True), "clear": ("clr", False)}, None, None, False),
        ("$dlatch", "EN_POLARITY=1", "EN=en D=3'011", True,
         {"async_reset": ("en", True)}, "011", None, False),
        ("$dffsr", "CLK_POLARITY=0 SET_POLARITY=0 CLR_POLARITY=1", "CLK=clk SET=set CLR=clr D=d", False,
         {"clock": ("clk", False), "set": ("set", False), "clear": ("clr", True)}, None, None, False),
        ("$dff", "CLK_POLARITY=0", "CLK=clk D=d", False,
         {"clock": ("clk", False)}, None, None, False),
        ("$adffe", "CLK_POLARITY=0 EN_POLARITY=1 ARST_POLARITY=0 ARST_VALUE=3'101", "CLK=clk EN=en ARST=rst D=d", False,
         {"clock": ("clk", False), "enable": ("en", True), "async_reset": ("rst", False)}, "101", None, False),
        ("$sdffe", "CLK_POLARITY=1 EN_POLARITY=0 SRST_POLARITY=0 SRST_VALUE=3'011", "CLK=clk EN=en SRST=rst D=d", False,
         {"clock": ("clk", True), "enable": ("en", False), "sync_reset": ("rst", False)}, None, "011", False),
        ("$sdffce", "CLK_POLARITY=1 EN_POLARITY=0 SRST_POLARITY=0 SRST_VALUE=3'011", "CLK=clk EN=en SRST=rst D=d", False,
         {"clock": ("clk", True), "enable": ("en", False), "sync_reset": ("rst", False)}, None, "011", True),
    ]
    for kind, set_positive, clear_positive in (("$_SR_NN_", False, False), ("$_SR_NP_", False, True),
                                               ("$_SR_PN_", True, False), ("$_SR_PP_", True, True)):
        cases.append((kind, "", "S=set R=clr", True,
                      {"set": ("set", set_positive), "clear": ("clr", clear_positive)}, None, None, False))
    cases.append(("$_DFFSR_PNP_", "", "C=clk S=set R=clr D=d", False,
                  {"clock": ("clk", True), "set": ("set", False), "clear": ("clr", True)}, None, None, False))
    for index, (kind, parameters, pins, latch, controls, arst, srst, priority) in enumerate(cases):
        fine = kind.startswith("$_")
        width = 1 if fine else 3
        lines = ["autoidx 1", "attribute \\top 1", "module \\top"]
        widths = {"clk": 1, "en": 1, "rst": 1, "set": width, "clr": width, "d": width}
        for port, (name, size) in enumerate(widths.items(), 1):
            lines.append(f"  wire width {size} input {port} \\{name}")
        lines += [f"  attribute \\init {width}'" + ("1" if fine else "101"),
                  f"  wire width {width} output 7 \\q", f"  cell {kind} \\state"]
        if not fine:
            lines.append("    parameter \\WIDTH 3")
        for assignment in parameters.split():
            key, value = assignment.split("=")
            lines.append(f"    parameter \\{key} {value}")
        for assignment in (pins + " Q=q").split():
            key, value = assignment.split("=")
            lines.append(f"    connect \\{key} " + (value if "'" in value else "\\" + value))
        lines += ["  end", "end"]
        source = work / f"semantics{index}.il"
        source.write_text("\n".join(lines) + "\n")
        run(f"read_rtlil {source}; cellfilter; cellinstrument -plan-only -manifest {work}/static-plan.json; "
            f"cellinstrument -manifest {work}/static-actual.json")
        for stage in ("plan", "actual"):
            report = json.loads((work / f"static-{stage}.json").read_text())
            info, = report["registers"]
            context = kind, index, stage
            assert info["latch"] is latch, ("latch identity", context, info)
            assert info["fine"] is fine, context
            for flag, control in (("has_clk", "clock"), ("has_ce", "enable"), ("has_aload", "async_load"),
                                  ("has_arst", "async_reset"), ("has_srst", "sync_reset"), ("has_sr", "set")):
                assert info[flag] == (control in controls), ("storage flag", flag, context)
            assert info["ce_over_srst"] is priority, context
            assert set(info["controls"]) == set(controls), context
            for control, (wire, positive) in controls.items():
                expected = [["\\" + wire, bit] for bit in range(widths[wire])]
                assert info["controls"][control] == {"source": expected, "positive": positive}, ("control polarity/source", control, context)
            for field, bits in (("init", "1" if fine else "101"), ("arst_value", arst), ("srst_value", srst)):
                expected = bits or ""
                assert info[field]["bits"] == expected and info[field]["width"] == len(expected), (field, context)
            assert info["async_data"] == ([["\\d", bit] for bit in range(width)] if "async_load" in controls else []), context
            item, = report["items"]
            assert item["role"] == "register_q" and item["port"] == "\\Q", context
            assert item["source"] == [["\\q", bit] for bit in range(width)], context
