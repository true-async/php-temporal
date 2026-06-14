<?php

/** Shared helpers for tests that need a live Temporal frontend. */

function temporal_test_address(): string
{
    return getenv('TEMPORAL_ADDRESS') ?: '127.0.0.1:7233';
}

function temporal_test_namespace(): string
{
    return getenv('TEMPORAL_NAMESPACE') ?: 'default';
}

/** Skip the test unless a Temporal frontend accepts a TCP connection. */
function temporal_skip_if_no_server(): void
{
    [$host, $port] = explode(':', temporal_test_address());
    $conn = @fsockopen($host, (int) $port, $errno, $errstr, 1.0);

    if ($conn === false) {
        die('skip no Temporal server at ' . temporal_test_address());
    }

    fclose($conn);
}
