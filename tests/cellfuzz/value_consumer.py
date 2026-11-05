"""Test-only manifest consumer; bit strings are MSB first, as eval/VCD print.

This module reads actual block values. It never produces packed values from
item sources and does not import the production planner or test RTLIL plugin.
"""


def decode_items(report, block_values):
    def require(condition, reason):
        if not condition:
            raise ValueError(reason)

    require(report["schema_version"] == 1 and report["status"] == "materialized", "materialized schema v1 required")
    items, transport = report["items"], report["transport"]
    require([item["monitor_id"] for item in items] == list(range(len(items))), "invalid monitor IDs")
    require(all(item["width"] > 0 for item in items), "invalid item width")
    ports = {port["name"]: port["width"] for port in transport["ports"]}
    require(len(ports) == len(transport["ports"]) and all(width > 0 for width in ports.values()), "invalid ports")
    require(block_values.keys() == ports.keys(), "block names differ from manifest")
    for name, width in ports.items():
        require(len(block_values[name]) == width and set(block_values[name]) <= set("01xz"), "invalid block value")
    decoded = [[None] * item["width"] for item in items]
    targets = {name: [False] * width for name, width in ports.items()}
    for segment in transport["segments"]:
        monitor, item_lsb, name, port_lsb, width = (segment[key] for key in
                                                  ("monitor_id", "item_lsb", "port_name", "port_lsb", "width"))
        require(0 <= monitor < len(items) and name in ports, "unknown item or port")
        require(width > 0 and item_lsb >= 0 and item_lsb + width <= len(decoded[monitor]) and
                port_lsb >= 0 and port_lsb + width <= ports[name], "segment out of range")
        for offset in range(width):
            source_bit, target_bit = item_lsb + offset, port_lsb + offset
            require(decoded[monitor][source_bit] is None and not targets[name][target_bit], "overlapping segments")
            decoded[monitor][source_bit] = block_values[name][-1 - target_bit]
            targets[name][target_bit] = True
    require(all(all(bit is not None for bit in item) for item in decoded) and
            all(all(bits) for bits in targets.values()), "incomplete coverage")
    require(sum(map(len, decoded)) == sum(ports.values()) == transport["total_bits"], "total width mismatch")
    return ["".join(reversed(item)) for item in decoded]
