#!/usr/bin/env python3
"""NE102 bench MQTT broker (amqtt) on 0.0.0.0:9991 - run and forget."""
import asyncio
from amqtt.broker import Broker

config = {
    "listeners": {
        "default": {"type": "tcp", "bind": "0.0.0.0:9991", "max_connections": 10},
    },
    "sys_interval": 10,
    "auth": {"allow-anonymous": True},
    "topic-check": {"enabled": False},
}

async def main():
    b = Broker(config)
    await b.start()
    print("amqtt broker up on 0.0.0.0:9991", flush=True)
    await asyncio.Event().wait()

asyncio.run(main())
