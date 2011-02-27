#include "../../../stdafx.h"

#include "video_decoder.h"

#include <common/memory/safe_ptr.h>

#include <core/video_format.h>

#include <mixer/frame/draw_frame.h>
#include <mixer/image/image_transform.h>

#include <tbb/parallel_for.h>

#include <algorithm>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#define __STDC_CONSTANT_MACROS
	#define __STDC_LIMIT_MACROS
	#include <libswscale/swscale.h>
	#include <libavformat/avformat.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

namespace caspar { namespace core { namespace ffmpeg {
	
pixel_format::type get_pixel_format(PixelFormat pix_fmt)
{
	switch(pix_fmt)
	{
		case PIX_FMT_BGRA:		return pixel_format::bgra;
		case PIX_FMT_ARGB:		return pixel_format::argb;
		case PIX_FMT_RGBA:		return pixel_format::rgba;
		case PIX_FMT_ABGR:		return pixel_format::abgr;
		case PIX_FMT_YUV444P:	return pixel_format::ycbcr;
		case PIX_FMT_YUV422P:	return pixel_format::ycbcr;
		case PIX_FMT_YUV420P:	return pixel_format::ycbcr;
		case PIX_FMT_YUV411P:	return pixel_format::ycbcr;
		case PIX_FMT_YUV410P:	return pixel_format::ycbcr;
		case PIX_FMT_YUVA420P:	return pixel_format::ycbcra;
		default:				return pixel_format::invalid;
	}
}

pixel_format_desc get_pixel_format_desc(PixelFormat pix_fmt, size_t width, size_t height)
{
	// Get linesizes
	AVPicture dummy_pict;	
	avpicture_fill(&dummy_pict, nullptr, pix_fmt, width, height);

	pixel_format_desc desc;
	desc.pix_fmt = get_pixel_format(pix_fmt);
		
	switch(desc.pix_fmt)
	{
	case pixel_format::bgra:
	case pixel_format::argb:
	case pixel_format::rgba:
	case pixel_format::abgr:
		{
			desc.planes.push_back(pixel_format_desc::plane(dummy_pict.linesize[0]/4, height, 4));						
			return desc;
		}
	case pixel_format::ycbcr:
	case pixel_format::ycbcra:
		{		
			// Find chroma height
			size_t size2 = dummy_pict.data[2] - dummy_pict.data[1];
			size_t h2 = size2/dummy_pict.linesize[1];			

			desc.planes.push_back(pixel_format_desc::plane(dummy_pict.linesize[0], height, 1));
			desc.planes.push_back(pixel_format_desc::plane(dummy_pict.linesize[1], h2, 1));
			desc.planes.push_back(pixel_format_desc::plane(dummy_pict.linesize[2], h2, 1));

			if(desc.pix_fmt == pixel_format::ycbcra)						
				desc.planes.push_back(pixel_format_desc::plane(dummy_pict.linesize[3], height, 1));	
			return desc;
		}		
	default:		
		desc.pix_fmt = pixel_format::invalid;
		return desc;
	}
}

struct video_decoder::implementation : boost::noncopyable
{	
	std::shared_ptr<frame_factory> frame_factory_;
	std::shared_ptr<SwsContext> sws_context_;

	AVCodecContext* codec_context_;

	const int width_;
	const int height_;
	const PixelFormat pix_fmt_;
	pixel_format_desc desc_;

public:
	explicit implementation(AVCodecContext* codec_context, const safe_ptr<frame_factory>& frame_factory) 
		: frame_factory_(frame_factory)
		, codec_context_(codec_context)
		, width_(codec_context_->width)
		, height_(codec_context_->height)
		, pix_fmt_(codec_context_->pix_fmt)
		, desc_(get_pixel_format_desc(pix_fmt_, width_, height_))
	{
		double frame_time = static_cast<double>(codec_context_->time_base.num) / static_cast<double>(codec_context_->time_base.den);
		double format_frame_time = 1.0/frame_factory->get_video_format_desc().fps;
		if(abs(frame_time - format_frame_time) > 0.0001)
			BOOST_THROW_EXCEPTION(file_read_error() << msg_info("Invalid video framerate.") << arg_value_info(boost::lexical_cast<std::string>(frame_time)));

		if(desc_.pix_fmt == pixel_format::invalid)
		{
			CASPAR_LOG(warning) << "Hardware accelerated color transform not supported.";

			desc_ = get_pixel_format_desc(PIX_FMT_BGRA, width_, height_);
			double param;
			sws_context_.reset(sws_getContext(width_, height_, pix_fmt_, width_, height_, PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, &param), sws_freeContext);
			if(!sws_context_)
				BOOST_THROW_EXCEPTION(operation_failed() <<
									  msg_info("Could not create software scaling context.") << 
									  boost::errinfo_api_function("sws_getContext"));
		}
	}
	
	safe_ptr<write_frame> execute(const aligned_buffer& video_packet)
	{				
		safe_ptr<AVFrame> decoded_frame(avcodec_alloc_frame(), av_free);

		int frame_finished = 0;
		const int result = avcodec_decode_video(codec_context_, decoded_frame.get(), &frame_finished, video_packet.data(), video_packet.size());
		
		if(result < 0)
			BOOST_THROW_EXCEPTION(invalid_operation() << msg_info("avcodec_decode_video failed"));
		
		auto write = frame_factory_->create_frame(desc_);
		if(sws_context_ == nullptr)
		{
			tbb::parallel_for(0, static_cast<int>(desc_.planes.size()), 1, [&](int n)
			{
				auto plane            = desc_.planes[n];
				auto result           = write->image_data(n).begin();
				auto decoded          = decoded_frame->data[n];
				auto decoded_linesize = decoded_frame->linesize[n];
				
				tbb::parallel_for(0, static_cast<int>(desc_.planes[n].height), 1, [&](int y)
				{
					std::copy_n(decoded + y*decoded_linesize, plane.linesize, result + y*plane.linesize);
				});
			});
		}
		else
		{
			safe_ptr<AVFrame> av_frame(avcodec_alloc_frame(), av_free);	
			avcodec_get_frame_defaults(av_frame.get());			
			avpicture_fill(reinterpret_cast<AVPicture*>(av_frame.get()), write->image_data().begin(), PIX_FMT_BGRA, width_, height_);
		 
			sws_scale(sws_context_.get(), decoded_frame->data, decoded_frame->linesize, 0, height_, av_frame->data, av_frame->linesize);	
		}	

		if(codec_context_->codec_id == CODEC_ID_DVVIDEO && frame_factory_->get_video_format_desc().mode == video_mode::upper)
			write->get_image_transform().set_image_translation(0.0f, 1.0/static_cast<double>(height_));

		return write;
	}
};

video_decoder::video_decoder(AVCodecContext* codec_context, const safe_ptr<frame_factory>& frame_factory) : impl_(new implementation(codec_context, frame_factory)){}
safe_ptr<write_frame> video_decoder::execute(const aligned_buffer& video_packet){return impl_->execute(video_packet);}
}}}