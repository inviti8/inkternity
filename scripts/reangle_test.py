#!/usr/bin/env python3
"""
Phase 0 test client for the AI camera-angle ("reangle") feature.

Posts an input sketch + a ComfyUI workflow to a RunPod Serverless endpoint
(the `worker-comfyui` image), polls until done, and saves the returned image.
This is both the validation harness for Phase 0 AND the reference for the HTTP
contract Inkternity will implement in Phase 1.

Prereqs:
  - A RunPod Serverless endpoint running `worker-comfyui` with the Qwen-Image-Edit
    + Multiple-Angles LoRA + Lightning models loaded (see AI_CAMERA_ANGLE_IMPLEMENTATION.md).
  - RUNPOD_API_KEY in the repo-root .env (already present) or the environment.
  - A ComfyUI workflow exported in **API format** (ComfyUI: Settings -> enable dev
    mode -> "Save (API Format)"). In that workflow, put the literal token __PROMPT__
    in your positive CLIPTextEncode text, and make the LoadImage node load a file
    named exactly like --image-node-name (default: input_image.png).

Run with uv (per repo convention):
  uv run python scripts/reangle_test.py \
      --endpoint <RUNPOD_ENDPOINT_ID> \
      --workflow workflow_api.json \
      --image "C:/Users/surfa/Pictures/ai-camera-angle-tests/original.png" \
      --prompt "same character design, flat 2D pencil sketch line art, profile view" \
      --out reangled.png

Stdlib-only (no pip installs) so `uv run` works with zero dependencies.
"""

import argparse
import base64
import json
import os
import sys
import time
import urllib.request
import urllib.error

RUNPOD_BASE = "https://api.runpod.ai/v2"


def load_api_key() -> str:
    """RUNPOD_API_KEY from the environment, else parsed from repo-root .env.
    We only ever read this one variable; we never print its value."""
    key = os.environ.get("RUNPOD_API_KEY")
    if key:
        return key.strip()
    env_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".env")
    try:
        with open(env_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line.startswith("RUNPOD_API_KEY="):
                    return line.split("=", 1)[1].strip().strip('"').strip("'")
    except FileNotFoundError:
        pass
    sys.exit("RUNPOD_API_KEY not found in environment or .env")


def http_post(url: str, payload: dict, api_key: str) -> dict:
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, method="POST")
    req.add_header("Authorization", f"Bearer {api_key}")
    req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=120) as resp:
        return json.loads(resp.read().decode("utf-8"))


def http_get(url: str, api_key: str) -> dict:
    req = urllib.request.Request(url, method="GET")
    req.add_header("Authorization", f"Bearer {api_key}")
    with urllib.request.urlopen(req, timeout=120) as resp:
        return json.loads(resp.read().decode("utf-8"))


def main() -> None:
    ap = argparse.ArgumentParser(description="RunPod reangle Phase-0 test client")
    ap.add_argument("--endpoint", required=True, help="RunPod Serverless endpoint ID")
    ap.add_argument("--workflow", required=True, help="ComfyUI workflow, API format (json)")
    ap.add_argument("--image", required=True, help="Input sketch (png/jpg/webp)")
    ap.add_argument("--out", default="reangled.png", help="Where to save the result")
    ap.add_argument("--prompt", default="", help="Replaces the __PROMPT__ token in the workflow")
    ap.add_argument("--image-node-name", default="input_image.png",
                    help="Filename the workflow's LoadImage node expects")
    ap.add_argument("--poll", type=float, default=2.0, help="Status poll interval (s)")
    ap.add_argument("--timeout", type=float, default=600.0, help="Overall timeout (s)")
    args = ap.parse_args()

    api_key = load_api_key()

    with open(args.workflow, "r", encoding="utf-8") as f:
        workflow_str = f.read()
    if args.prompt:
        workflow_str = workflow_str.replace("__PROMPT__", args.prompt.replace('"', '\\"'))
    workflow = json.loads(workflow_str)

    with open(args.image, "rb") as f:
        img_b64 = base64.b64encode(f.read()).decode("ascii")

    payload = {
        "input": {
            "workflow": workflow,
            "images": [{"name": args.image_node_name, "image": img_b64}],
        }
    }

    run_url = f"{RUNPOD_BASE}/{args.endpoint}/run"
    print(f"-> POST {run_url}  (image + workflow)")
    t0 = time.time()
    job = http_post(run_url, payload, api_key)
    job_id = job.get("id")
    if not job_id:
        sys.exit(f"No job id in response: {json.dumps(job)[:400]}")
    print(f"   job {job_id} status={job.get('status')}")

    status_url = f"{RUNPOD_BASE}/{args.endpoint}/status/{job_id}"
    result = None
    while True:
        if time.time() - t0 > args.timeout:
            sys.exit("Timed out waiting for the job.")
        st = http_get(status_url, api_key)
        s = st.get("status")
        if s == "COMPLETED":
            result = st
            break
        if s in ("FAILED", "CANCELLED", "TIMED_OUT"):
            sys.exit(f"Job {s}: {json.dumps(st)[:800]}")
        print(f"   ... {s}  ({time.time() - t0:0.1f}s)")
        time.sleep(args.poll)

    # worker-comfyui returns produced images under output. Shape varies by config
    # (base64 or a URL); handle both, and dump the raw output if neither matches so
    # we can finalize the exact contract for Phase 1.
    out = result.get("output", {})
    images = out.get("images") if isinstance(out, dict) else None
    saved = False
    if images:
        first = images[0]
        if isinstance(first, dict) and first.get("data"):
            with open(args.out, "wb") as f:
                f.write(base64.b64decode(first["data"]))
            saved = True
        elif isinstance(first, dict) and first.get("image"):
            with open(args.out, "wb") as f:
                f.write(base64.b64decode(first["image"]))
            saved = True
        elif isinstance(first, str) and first.startswith("http"):
            with urllib.request.urlopen(first, timeout=120) as r, open(args.out, "wb") as f:
                f.write(r.read())
            saved = True

    dt = time.time() - t0
    if saved:
        print(f"[OK] saved {args.out}  in {dt:0.1f}s")
    else:
        # Don't fail silently — print the output so we can wire the exact field.
        print(f"[?] completed in {dt:0.1f}s but couldn't locate an image in output.")
        print(json.dumps(out, indent=2)[:2000])


if __name__ == "__main__":
    main()
