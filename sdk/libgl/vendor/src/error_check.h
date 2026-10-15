/* CiukiOS port: GL enums must be evaluated by C, not preprocessor #if. */
#ifndef RETVAL
#define RETVAL
#endif
#if TGL_FEATURE_ERROR_CHECK == 1
#ifdef ERROR_FLAG
{
    if (c->error_flag == GL_NO_ERROR) c->error_flag = ERROR_FLAG;
    return RETVAL;
}
#else
#if TGL_FEATURE_STRICT_OOM_CHECKS == 1
if (c->error_flag == GL_OUT_OF_MEMORY) return RETVAL;
#endif
#endif
#endif
#undef RETVAL
#undef ERROR_FLAG
