#!/usr/bin/env python3
"""AIME 2025 evaluation script for ninfer.

Sends each AIME problem to the ninfer server with thinking enabled,
extracts the boxed answer, and reports the score.

Usage:
    python3 aime_eval.py [--url URL] [--kv-dtype int8|bf16] [--temperature 0]
"""

import json
import re
import sys
import time
import argparse
import requests


def load_aime():
    """Load AIME 2025 dataset."""
    from datasets import load_dataset
    ds = load_dataset("math-ai/aime25")
    return list(ds["test"])


def extract_answer(text):
    """Extract the integer from \\boxed{N} in the response."""
    # Find all \boxed{...} occurrences, take the last one
    matches = re.findall(r'\\boxed\{(\d+)\}', text)
    if matches:
        return int(matches[-1])
    # Fallback: look for "answer is N" or "N" near the end
    matches = re.findall(r'(?:answer is|=)\s*(\d{1,3})', text[-500:])
    if matches:
        return int(matches[-1])
    return None


def send_problem(url, problem, temperature=0, max_tokens=262144):
    """Send a single AIME problem to the server."""
    payload = {
        "model": "qwen3.8-27b",
        "messages": [
            {"role": "system", "content": "You are a mathematical reasoning assistant. Solve the problem step by step. Put your final answer as an integer in \\boxed{}."},
            {"role": "user", "content": problem}
        ],
        "max_tokens": max_tokens,
        "stream": False,
        "temperature": temperature,
        "top_p": 1.0 if temperature == 0 else 0.8,
    }

    start = time.time()
    try:
        response = requests.post(url, json=payload, timeout=3600)
    except requests.exceptions.ReadTimeout:
        print(f"  TIMEOUT after 3600s")
        return None, 3600
    elapsed = time.time() - start

    if response.status_code != 200:
        print(f"  ERROR: HTTP {response.status_code}: {response.text[:200]}")
        return None, elapsed

    data = response.json()
    content = data.get("choices", [{}])[0].get("message", {}).get("content", "")
    gen_tokens = data.get("usage", {}).get("completion_tokens", 0)
    finish = data.get("choices", [{}])[0].get("finish_reason", "?")

    answer = extract_answer(content)
    return {
        "answer": answer,
        "expected": None,  # filled by caller
        "content_len": len(content),
        "gen_tokens": gen_tokens,
        "finish": finish,
        "elapsed": elapsed,
    }, elapsed


def run_eval(url, temperature, max_rows=None, skip_ids=None):
    """Run the full AIME 2025 evaluation."""
    problems = load_aime()
    if max_rows:
        problems = problems[:max_rows]

    # Load previous results if resuming
    prev_results = {}
    prev_correct = 0
    if skip_ids:
        try:
            with open("/tmp/aime_results.json") as f:
                prev = json.load(f)
            for r in prev["results"]:
                if r["id"] in skip_ids:
                    prev_results[r["id"]] = r
                    if r["correct"]:
                        prev_correct += 1
            print(f"Resuming: skipping {len(prev_results)} already-completed problems")
        except:
            pass

    results = []
    correct = prev_correct
    total = len(problems)

    print(f"AIME 2025 Evaluation")
    print(f"URL: {url}")
    print(f"Problems: {total}")
    print(f"Temperature: {temperature}")
    print(f"{'='*60}")

    for i, p in enumerate(problems):
        expected = int(p["answer"])

        # Skip already-completed problems
        if p["id"] in prev_results:
            r = prev_results[p["id"]]
            status = "CORRECT" if r["correct"] else "WRONG"
            got_str = str(r.get("answer", "None"))
            print(f"[{i+1}/{total}] Problem {p['id']} (answer: {expected}) [CACHED] {status}: got={got_str}")
            results.append(r)
            continue

        print(f"[{i+1}/{total}] Problem {p['id']} (answer: {expected})...")

        result, elapsed = send_problem(url, p["problem"], temperature)
        if result is None:
            print(f"  FAILED (no response)")
            results.append({"id": p["id"], "expected": expected, "got": None, "correct": False, "elapsed": 0})
            continue

        result["expected"] = expected
        got = result["answer"]
        is_correct = got == expected
        if is_correct:
            correct += 1

        status = "CORRECT" if is_correct else "WRONG"
        got_str = str(got) if got is not None else "None"
        print(f"  {status}: got={got_str} expected={expected} "
              f"tokens={result['gen_tokens']} time={elapsed:.1f}s finish={result['finish']}")

        result["id"] = p["id"]
        result["correct"] = is_correct
        results.append(result)

    score = correct / total * 100
    print(f"\n{'='*60}")
    print(f"Score: {correct}/{total} = {score:.1f}%")
    print(f"Total time: {sum(r['elapsed'] for r in results):.1f}s")

    # Save results
    with open("/tmp/aime_results.json", "w") as f:
        json.dump({"score": score, "correct": correct, "total": total, "results": results}, f, indent=2)
    print(f"Results saved to /tmp/aime_results.json")

    return score


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="AIME 2025 evaluation for ninfer")
    parser.add_argument("--url", default="http://strix.lan:8080/v1/chat/completions",
                        help="Server URL")
    parser.add_argument("--temperature", type=float, default=0,
                        help="Sampling temperature (0=greedy)")
    parser.add_argument("--max-rows", type=int, default=None,
                        help="Max problems to run (default: all 30)")
    parser.add_argument("--skip-ids", default=None,
                        help="Comma-separated problem IDs to skip (resume mode)")
    args = parser.parse_args()

    skip_ids = set(args.skip_ids.split(",")) if args.skip_ids else None
    run_eval(args.url, args.temperature, args.max_rows, skip_ids)
