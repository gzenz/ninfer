#!/usr/bin/env python3
"""Harder needle-in-haystack test for KV cache quality evaluation.

Multiple needles with distractors, realistic context, and multi-hop questions.
Designed to stress attention quality, not just pattern matching.

Usage:
    python3 needle_hard.py [--url URL] [--depths 64k,128k,200k]
"""

import json
import re
import time
import argparse
import requests
import random
import string


# Multiple needles with similar structure but different values
NEEDLES = [
    {"id": "alpha", "fact": "The alpha configuration parameter is set to 47.", "question": "What is the alpha configuration parameter set to?", "answer": "47"},
    {"id": "beta", "fact": "The beta configuration parameter is set to 92.", "question": "What is the beta configuration parameter set to?", "answer": "92"},
    {"id": "gamma", "fact": "The gamma configuration parameter is set to 158.", "question": "What is the gamma configuration parameter set to?", "answer": "158"},
    {"id": "delta", "fact": "The delta configuration parameter is set to 203.", "question": "What is the delta configuration parameter set to?", "answer": "203"},
    {"id": "epsilon", "fact": "The epsilon configuration parameter is set to 341.", "question": "What is the epsilon configuration parameter set to?", "answer": "341"},
]

# Distractors: similar-looking facts with wrong values
DISTRACTORS = [
    "The alpha configuration parameter was previously set to 12.",
    "The beta configuration parameter defaults to 55.",
    "The gamma configuration parameter was tested at 89.",
    "The delta configuration parameter can range from 100 to 300.",
    "The epsilon configuration parameter was set to 177 in the old config.",
    "Note: the alpha parameter should not exceed 50 in production.",
    "The beta parameter was 92 in version 1, but changed in version 2.",
    "Historical value of gamma: 158 (deprecated).",
    "The delta parameter is unrelated to the epsilon parameter.",
    "Previous epsilon readings: 341, 341, 340, 341 (stable).",
]


def generate_code_haystack(target_tokens):
    """Generate realistic-looking code/text context."""
    target_chars = target_tokens * 4
    parts = []
    chars = 0

    functions = [
        "def process_data", "class DataHandler", "func main()",
        "async function fetch", "public void handle", "int compute",
        "void initialize", "def validate", "struct Config", "enum Status",
    ]

    while chars < target_chars:
        fn = random.choice(functions)
        lines = []
        lines.append(f"{fn}() {{")
        for _ in range(random.randint(5, 15)):
            var = ''.join(random.choices(string.ascii_lowercase, k=random.randint(4, 10)))
            val = random.randint(0, 999)
            op = random.choice(["=", "+=", "-=", "*="])
            lines.append(f"    {var} {op} {val};")
        lines.append("}")
        # Add some comments and strings
        for _ in range(random.randint(2, 5)):
            comment = ' '.join(random.choices(string.ascii_lowercase + " ", k=random.randint(10, 40)))
            lines.append(f"    // {comment}")
        block = "\n".join(lines) + "\n\n"
        parts.append(block)
        chars += len(block)

    return "\n".join(parts)


def build_hard_prompt(total_tokens, needle_positions):
    """Build a prompt with multiple needles and distractors at specific positions.

    needle_positions: dict of {needle_id: fraction (0-1)} for insertion depth.
    """
    needle_tokens = sum(len(n["fact"]) // 4 for n in NEEDLES)
    distractor_tokens = sum(len(d) // 4 for d in DISTRACTORS)
    question_tokens = 100  # buffer
    filler_tokens = total_tokens - needle_tokens - distractor_tokens - question_tokens - 200

    if filler_tokens < 1000:
        raise ValueError(f"Total tokens too small: {total_tokens}")

    # Build the haystack in segments, inserting needles at specified positions
    # Split filler into segments around needle positions
    positions = sorted(set(needle_positions.values()))
    segments = []
    prev_frac = 0.0
    for frac in positions:
        seg_frac = frac - prev_frac
        seg_tokens = int(filler_tokens * seg_frac)
        segments.append(generate_code_haystack(seg_tokens))
        prev_frac = frac
    # Final segment
    seg_tokens = int(filler_tokens * (1.0 - prev_frac))
    segments.append(generate_code_haystack(seg_tokens))

    # Interleave segments with needles at the right positions
    prompt_parts = []
    seg_idx = 0
    for frac in positions:
        prompt_parts.append(segments[seg_idx])
        seg_idx += 1
        # Insert needles at this position
        for nid, nfrac in needle_positions.items():
            if nfrac == frac:
                needle = next(n for n in NEEDLES if n['id'] == nid)
                prompt_parts.append(f"\n[CONFIG] {needle['fact']}\n")
        # Insert some distractors too
        for d in random.sample(DISTRACTORS, min(2, len(DISTRACTORS))):
            prompt_parts.append(f"\n[NOTE] {d}\n")

    prompt_parts.append(segments[seg_idx])  # final segment

    prompt = "\n".join(prompt_parts)

    # Add distractors at random positions in the text
    for d in DISTRACTORS:
        pos = random.randint(0, len(prompt))
        prompt = prompt[:pos] + f"\n[NOTE] {d}\n" + prompt[pos:]

    return prompt


def build_multihop_prompt(total_tokens):
    """Build a prompt requiring multi-hop reasoning.

    The answer requires combining: needle A says "the code is in file X",
    needle B says "file X contains the number Y". The answer is Y.
    """
    needle_a = "The secret database connection string is stored in the file named 'vault_config.dat'."
    needle_b = "In vault_config.dat, the port number is 8291."
    question = "What port number is the secret database connection string stored at? Reply with only the number."

    filler_tokens = total_tokens - len(needle_a)//4 - len(needle_b)//4 - len(question)//4 - 200
    if filler_tokens < 1000:
        raise ValueError(f"Too small: {total_tokens}")

    # Insert needle A at 25% and needle B at 75% (far apart)
    haystack = generate_code_haystack(filler_tokens)
    pos_a = len(haystack) // 4
    pos_b = (len(haystack) * 3) // 4

    prompt = (haystack[:pos_a] +
              f"\n[LOG] {needle_a}\n" +
              haystack[pos_a:pos_b] +
              f"\n[LOG] {needle_b}\n" +
              haystack[pos_b:] +
              f"\n\n{question}")

    return prompt


def run_hard_needle(url, depths):
    """Run the harder needle test."""
    results = []

    print(f"Hard Needle-in-Haystack Test")
    print(f"URL: {url}")
    print(f"Depths: {depths}")
    print(f"{'='*70}")

    # Test 1: Multiple needles with distractors
    print(f"\n--- Test 1: Multiple needles with distractors ---")
    for depth_tokens in depths:
        # Place each needle at a different depth
        needle_positions = {
            "alpha": 0.10,
            "beta": 0.30,
            "gamma": 0.50,
            "delta": 0.70,
            "epsilon": 0.90,
        }

        for nid in ["alpha", "beta", "gamma", "delta", "epsilon"]:
            needle = next(n for n in NEEDLES if n["id"] == nid)
            label = f"{depth_tokens//1000}k/{nid}"
            print(f"\n[{label}] Building prompt...")

            try:
                prompt = build_hard_prompt(depth_tokens, needle_positions)
            except ValueError as e:
                print(f"  SKIP: {e}")
                results.append({"test": "multi", "depth": depth_tokens, "needle": nid, "found": False, "error": str(e)})
                continue

            payload = {
                "model": "qwen3.8-27b",
                "messages": [{"role": "user", "content": prompt + f"\n\n{needle['question']} Reply with only the number, nothing else."}],
                "max_tokens": 2000,
                "stream": False,
                "temperature": 0,
                "top_p": 1.0,
                "extra_body": {"chat_template_kwargs": {"enable_thinking": False}},
            }

            start = time.time()
            try:
                response = requests.post(url, json=payload, timeout=3600)
                elapsed = time.time() - start
            except requests.exceptions.ReadTimeout:
                print(f"  TIMEOUT")
                results.append({"test": "multi", "depth": depth_tokens, "needle": nid, "found": False, "elapsed": 3600})
                continue

            if response.status_code != 200:
                print(f"  ERROR: HTTP {response.status_code}")
                results.append({"test": "multi", "depth": depth_tokens, "needle": nid, "found": False, "elapsed": elapsed, "error": f"HTTP {response.status_code}"})
                continue

            data = response.json()
            content = data.get("choices", [{}])[0].get("message", {}).get("content", "")
            prompt_tokens = data.get("usage", {}).get("prompt_tokens", 0)
            found = needle["answer"] in content
            status = "FOUND" if found else "MISSED"

            print(f"  {status}: got='{content.strip()[:80]}' expected={needle['answer']} prompt_tokens={prompt_tokens} time={elapsed:.1f}s")
            results.append({
                "test": "multi",
                "depth": depth_tokens,
                "needle": nid,
                "found": found,
                "response": content.strip()[:200],
                "expected": needle["answer"],
                "prompt_tokens": prompt_tokens,
                "elapsed": elapsed,
            })

    # Test 2: Multi-hop reasoning
    print(f"\n--- Test 2: Multi-hop reasoning (needle A at 25%, needle B at 75%) ---")
    for depth_tokens in depths:
        label = f"{depth_tokens//1000}k/multihop"
        print(f"\n[{label}] Building prompt...")

        try:
            prompt = build_multihop_prompt(depth_tokens)
        except ValueError as e:
            print(f"  SKIP: {e}")
            results.append({"test": "multihop", "depth": depth_tokens, "found": False, "error": str(e)})
            continue

        payload = {
            "model": "qwen3.8-27b",
            "messages": [{"role": "user", "content": prompt}],
            "max_tokens": 2000,
            "stream": False,
            "temperature": 0,
            "top_p": 1.0,
            "extra_body": {"chat_template_kwargs": {"enable_thinking": False}},
        }

        start = time.time()
        try:
            response = requests.post(url, json=payload, timeout=3600)
            elapsed = time.time() - start
        except requests.exceptions.ReadTimeout:
            print(f"  TIMEOUT")
            results.append({"test": "multihop", "depth": depth_tokens, "found": False, "elapsed": 3600})
            continue

        if response.status_code != 200:
            print(f"  ERROR: HTTP {response.status_code}")
            results.append({"test": "multihop", "depth": depth_tokens, "found": False, "elapsed": elapsed, "error": f"HTTP {response.status_code}"})
            continue

        data = response.json()
        content = data.get("choices", [{}])[0].get("message", {}).get("content", "")
        prompt_tokens = data.get("usage", {}).get("prompt_tokens", 0)
        found = "8291" in content
        status = "FOUND" if found else "MISSED"

        print(f"  {status}: got='{content.strip()[:80]}' expected=8291 prompt_tokens={prompt_tokens} time={elapsed:.1f}s")
        results.append({
            "test": "multihop",
            "depth": depth_tokens,
            "found": found,
            "response": content.strip()[:200],
            "expected": "8291",
            "prompt_tokens": prompt_tokens,
            "elapsed": elapsed,
        })

    # Summary
    print(f"\n{'='*70}")
    print(f"Summary:")

    print(f"\nTest 1: Multiple needles with distractors")
    for depth_tokens in depths:
        depth_results = [r for r in results if r["test"] == "multi" and r["depth"] == depth_tokens]
        found = sum(1 for r in depth_results if r.get("found"))
        total = len(depth_results)
        if total > 0:
            print(f"  {depth_tokens//1000}k: {found}/{total} found ({found/total*100:.0f}%)")

    print(f"\nTest 2: Multi-hop reasoning")
    for depth_tokens in depths:
        depth_results = [r for r in results if r["test"] == "multihop" and r["depth"] == depth_tokens]
        for r in depth_results:
            status = "FOUND" if r.get("found") else "MISSED"
            print(f"  {depth_tokens//1000}k: {status}")

    with open("/tmp/needle_hard_results.json", "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nResults saved to /tmp/needle_hard_results.json")

    return results


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Hard needle-in-haystack test for ninfer")
    parser.add_argument("--url", default="http://strix.lan:8080/v1/chat/completions",
                        help="Server URL")
    parser.add_argument("--depths", default="64000,128000,200000",
                        help="Comma-separated token depths to test")
    args = parser.parse_args()

    depths = [int(d) for d in args.depths.split(",")]
    run_hard_needle(args.url, depths)
