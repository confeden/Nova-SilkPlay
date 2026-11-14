"""Acceptance checks for a moving CFR source and an actively visible engine."""
def cadence_errors(result):
    errors=[]
    visibility=result.get('overlay_visibility',{})
    if not visibility.get('before') or not visibility.get('after'):
        errors.append('engine overlay was not visible at both recording boundaries')
    if visibility.get('samples',0)<=0 or visibility.get('hidden_samples',0)>0:
        errors.append('engine visibility was unverified or interrupted during recording')
    before=result.get('before',{});after=result.get('after',{})
    if before.get('paused',True) or after.get('paused',True):
        errors.append('source was paused')
    if after.get('t',0)-before.get('t',0)<result.get('capture_seconds',0)*.8:
        errors.append('source media clock did not advance enough')
    if after.get('total',0)<=before.get('total',0):
        errors.append('source decoded no frames')
    if after.get('dropped',0)>before.get('dropped',0):
        errors.append('source dropped frames')
    dda=result.get('dda',{})
    if dda.get('recorder_missed',0)>0:
        errors.append('recorder missed desktop updates')
    if dda.get('frames',0)<2:
        errors.append('recorder captured too few frames')
    return errors
