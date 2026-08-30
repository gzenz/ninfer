#!/usr/bin/env python3
"""Needle-in-haystack test for KV cache quality evaluation.

Inserts a specific fact at a position in a large context, then asks about it.
Tests at multiple depths to find where KV cache quantization degrades retrieval.

Usage:
    python3 needle_eval.py [--url URL] [--depths 32k,64k,128k,200k]
"""

import json
import re
import time
import argparse
import requests
import random
import string


NEEDLE = "The secret code for the vault is exactly 7293. Remember this number."
NEEDLE_QUESTION = "What is the secret code for the vault? Reply with only the number."


def generate_haystack(target_tokens):
    """Generate filler text approximately target_tokens long."""
    # ~4 chars per token, so target_chars = target_tokens * 4
    target_chars = target_tokens * 4

    # Use repetitive but varied text to avoid compression artifacts
    paragraphs = []
    chars_so_far = 0
    para_id = 0
    while chars_so_far < target_chars:
        para_id += 1
        words = []
        for _ in range(random.randint(50, 100)):
            word_len = random.randint(3, 12)
            word = ''.join(random.choices(string.ascii_lowercase, k=word_len))
            words.append(word)
        para = f"Section {para_id}: " + " ".join(words) + ". "
        paragraphs.append(para)
        chars_so_far += len(para)

    return "\n\n".join(paragraphs)


def build_needle_prompt(total_tokens, needle_depth_fraction):
    """Build a prompt with a needle at a specific depth."""
    needle_tokens = len(NEEDLE) // 4  # ~15 tokens
    question_tokens = len(NEEDLE_QUESTION) // 4  # ~15 tokens
    filler_tokens = total_tokens - needle_tokens - question_tokens - 100  # buffer

    if filler_tokens < 1000:
        raise ValueError(f"Total tokens too small: {total_tokens}")

    haystack = generate_haystack(filler_tokens)

    # Insert needle at the specified depth
    insert_pos = int(len(haystack) * needle_depth_fraction)
    before = haystack[:insert_pos]
    after = haystack[insert_pos:]

    prompt = f"{before}\n\n{NEEDLE}\n\n{after}\n\n{NEEDLE_QUESTION}"

    return prompt


def run_needle_test(url, depths, max_tokens=200):
    """Run needle-in-haystack at multiple depths."""
    results = []

    # Test at multiple depths: 0%, 25%, 50%, 75%, 100% of context
    depth_fractions = [0.0, 0.25, 0.50, 0.75, 0.95]

    print(f"Needle-in-Haystack Test")
    print(f"URL: {url}")
    print(f"Depths: {depths}")
    print(f"{'='*70}")

    for depth_tokens in depths:
        for frac in depth_fractions:
            label = f"{depth_tokens//1000}k @ {int(frac*100)}%"
            print(f"\n[{label}] Building prompt...")
            prompt = build_needle_prompt(depth_tokens, frac)

            payload = {
                "model": "qwen3.8-27b",
                "messages": [
                    {"role": "user", "content": prompt}
                ],
                "max_tokens": max_tokens,
                "stream": False,
                "temperature": 0,
                "top_p": 1.0,
            }

            start = time.time()
            try:
                response = requests.post(url, json=payload, timeout=3600)
                elapsed = time.time() - start
            except requests.exceptions.ReadTimeout:
                print(f"  TIMEOUT")
                results.append({"depth": depth_tokens, "fraction": frac, "found": False, "elapsed": 3600, "error": "timeout"})
                continue

            if response.status_code != 200:
                print(f"  ERROR: HTTP {response.status_code}")
                results.append({"depth": depth_tokens, "fraction": frac, "found": False, "elapsed": elapsed, "error": f"HTTP {response.status_code}"})
                continue

            data = response.json()
            content = data.get("choices", [{}])[0].get("message", {}).get("content", "")
            prompt_tokens = data.get("usage", {}).get("prompt_tokens", 0)

            # Check if the answer contains "7293"
            found = "7293" in content
            status = "FOUND" if found else "MISSED"

            print(f"  {status}: response='{content.strip()[:100]}' prompt_tokens={prompt_tokens} time={elapsed:.1f}s")
            results.append({
                "depth": depth_tokens,
                "fraction": frac,
                "found": found,
                "response": content.strip()[:200],
                "prompt_tokens": prompt_tokens,
                "elapsed": elapsed,
            })

    # Summary
    print(f"\n{'='*70}")
    print(f"Summary:")
    for depth_tokens in depths:
        depth_results = [r for r in results if r["depth"] == depth_tokens]
        found = sum(1 for r in depth_results if r["found"])
        total = len(depth_results)
        print(f"  {depth_tokens//1000}k: {found}/{total} found ({found/total*100:.0f}%)")

    with open("/tmp/needle_results.json", "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nResults saved to /tmp/needle_results.json")

    return results


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Needle-in-haystack test for ninfer")
    parser.add_argument("--url", default="http://strix.lan:8080/v1/chat/completions",
                        help="Server URL")
    parser.add_argument("--depths", default="32000,64000,128000,200000",
                        help="Comma-separated token depths to test")
    args = parser.parse_args()

    depths = [int(d) for d in args.depths.split(",")]
    run_needle_test(args.url, depths)
