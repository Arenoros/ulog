# Isolate routes by declared reliability

Every route explicitly declares Required or Best-Effort behavior. Retained work
for a Required Route applies backpressure to the pipeline and therefore invokes
its configured admission policy, while a Best-Effort Route may shed its own
encoded output without delaying independent routes; all losses remain visible
through exact route statistics and barrier results.
