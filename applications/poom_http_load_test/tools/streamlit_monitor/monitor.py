#!/usr/bin/env python3
"""
Simple Streamlit monitor for HTTP load-test targets.
"""

from __future__ import annotations

import datetime as dt
import time
from typing import Dict, List, Optional

import pandas as pd
import requests
import streamlit as st


def probe_target(url: str, timeout_s: float) -> Dict[str, object]:
    ts = dt.datetime.now()
    start = time.perf_counter()
    status_code: Optional[int] = None
    error_text = ""
    ok = False

    try:
        response = requests.get(url, timeout=timeout_s)
        status_code = response.status_code
        ok = 200 <= response.status_code < 500
    except Exception as exc:
        error_text = str(exc)
        ok = False

    latency_ms = (time.perf_counter() - start) * 1000.0
    return {
        "timestamp": ts,
        "ok": ok,
        "status_code": status_code if status_code is not None else 0,
        "latency_ms": latency_ms,
        "error": error_text,
    }


def to_dataframe(records: List[Dict[str, object]]) -> pd.DataFrame:
    if not records:
        return pd.DataFrame(columns=["timestamp", "ok", "status_code", "latency_ms", "error"])
    return pd.DataFrame.from_records(records)


def main() -> None:
    st.set_page_config(page_title="POOM HTTP Monitor", layout="wide")
    st.title("POOM HTTP Target Monitor")
    st.caption("Realtime status for your load-test target")

    if "running" not in st.session_state:
        st.session_state.running = False
    if "records" not in st.session_state:
        st.session_state.records = []

    with st.sidebar:
        st.subheader("Target")
        target_url = st.text_input("URL", value="http://192.168.3.89:8000/")
        timeout_s = st.slider("Timeout (s)", min_value=0.2, max_value=10.0, value=1.5, step=0.1)
        interval_ms = st.slider("Probe interval (ms)", min_value=100, max_value=5000, value=500, step=100)
        max_points = st.slider("History size", min_value=50, max_value=5000, value=600, step=50)

        col_a, col_b, col_c = st.columns(3)
        if col_a.button("Start", use_container_width=True):
            st.session_state.running = True
        if col_b.button("Stop", use_container_width=True):
            st.session_state.running = False
        if col_c.button("Clear", use_container_width=True):
            st.session_state.records = []

    records: List[Dict[str, object]] = st.session_state.records
    df = to_dataframe(records)

    c1, c2, c3, c4 = st.columns(4)
    total = len(df)
    up_count = int(df["ok"].sum()) if total > 0 else 0
    down_count = total - up_count
    availability = (100.0 * up_count / total) if total > 0 else 0.0
    last_status = "N/A"
    last_latency = 0.0

    if total > 0:
        last = df.iloc[-1]
        last_status = "UP" if bool(last["ok"]) else "DOWN"
        last_latency = float(last["latency_ms"])

    c1.metric("State", last_status)
    c2.metric("Availability", f"{availability:.2f}%")
    c3.metric("Checks", str(total))
    c4.metric("Last Latency", f"{last_latency:.1f} ms")

    if total > 0:
        chart_df = df.copy()
        chart_df["timestamp"] = pd.to_datetime(chart_df["timestamp"])
        chart_df = chart_df.set_index("timestamp")

        st.subheader("Latency (ms)")
        st.line_chart(chart_df["latency_ms"])

        st.subheader("Status History")
        show_df = df.copy()
        show_df["state"] = show_df["ok"].map({True: "UP", False: "DOWN"})
        st.dataframe(
            show_df[["timestamp", "state", "status_code", "latency_ms", "error"]].tail(200),
            use_container_width=True,
            hide_index=True,
        )
        st.caption(f"UP: {up_count} | DOWN: {down_count}")
    else:
        st.info("No probe data yet. Click Start in the sidebar.")

    if st.session_state.running:
        new_point = probe_target(target_url, timeout_s)
        records.append(new_point)
        if len(records) > max_points:
            del records[0 : len(records) - max_points]
        st.session_state.records = records
        time.sleep(interval_ms / 1000.0)
        st.rerun()


if __name__ == "__main__":
    main()
