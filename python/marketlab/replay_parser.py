import json
from dataclasses import dataclass
from typing import List, Optional, Dict, Any

@dataclass
class Event:
    event_type: str
    seq: int
    event_id: int
    raw: Dict[str, Any]

def parse_events(file_path: str) -> List[Event]:
    events = []
    with open(file_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            data = json.loads(line)
            events.append(Event(
                event_type=data.get("event", "UNKNOWN"),
                seq=data.get("seq", 0),
                event_id=data.get("event_id", 0),
                raw=data
            ))
    return events
